#include "stm32f10x.h"                  // Device header
#include "SD.h"
#include "Delay.h"                      //只用于ACMD41的重试间隔，见"超时"一节

/*==================== 配置 ====================*/

/*分频档位。【初始化阶段和数据阶段必须用不同速率】：
  规范要求卡在上电初始化阶段的时钟不超过400kHz，识别完成后则越快越好。
  72MHz / 256 = 281kHz（合规）；72MHz / 64 = 1.125MHz。
  跑通之后可以把快档试到 _32(2.25M) 甚至 _8(9M)——那是一次白送的信号完整性体检，
  不稳就退回来。*/
#define SD_SLOW_PRESCALER	SPI_BaudRatePrescaler_256
#define SD_FAST_PRESCALER	SPI_BaudRatePrescaler_64

#define SD_CS_PORT			GPIOA
#define SD_CS_PIN			GPIO_Pin_4

/*超时常量。★单位是这里最容易错100倍的地方：
  内层CPU空转每圈约0.3us，而"逐字节等待"每圈 = 一个完整字节时间
  （慢档28.4us / 快档7.1us），两者差约100倍。所以下面凡是以【字节】为单位的
  常量，含义是"最多再等多少个字节的时间"——这也正是SD规范自己的单位
  （NCR就是用字节时间定义的）。按CPU次数记账，会把该等466ms的地方写成等47秒。*/
#define SD_TMO_BYTE_CPU		0x8000u		//XferByte内TXE/RXNE，CPU次，约10ms
#define SD_TMO_R1			0x1000u		//R1响应。规范NCR≤8字节，这里给4096
										//（慢档4096×28.4us≈116ms）
#define SD_TMO_TOKEN		0x10000u	//数据令牌0xFE（快档65536×7.1us≈466ms，
										//规范读访问≤100ms）
#define SD_TMO_DATA_RESP	0x2000u		//写数据响应（≈58ms）
#define SD_TMO_BUSY			0x20000u	//写忙释放（≈931ms，规范单块写≤250ms）

/*ACMD41重试次数，每次间隔10ms → 约1s（规范上电初始化上限就是1s）。
  ★这一处【必须】用物理时间，不能改成循环计数：循环计数保证不了"至少10ms"，
  把10ms缩到1ms会让慢卡初始化不完，而且只在个别卡上偶发。*/
#define SD_ACMD41_RETRY		100u

/*==================== 内部状态 ====================*/

/*SPI层粘滞故障标志。字节级超时后置位，所有对外函数入口检查它并立即返回。
  没有它的话：SPE被清掉时，外层等R1的循环会做4096次 × 每次10ms字节超时
  = 41秒黑屏。有了它，整条链在第一次字节超时后就全线返回。*/
static uint8_t s_Fatal = 0;

static uint8_t s_IsV1  = 0;				//1=SD v1.x（CMD8报非法命令）
static uint8_t s_Csd[16];				//CSD原始16字节
static uint8_t s_Work[512];				//读工作缓冲 / 写测试的存档
static uint8_t s_Verify[512];			//写测试的图案与读回缓冲

SD_InfoDef SD_Info;						//对上层只读，不需要volatile（见SD.h）

/*==================== 底层：片选与字节收发 ====================*/

/**
  * @brief  SPI收发一个字节
  * @param  Data 要发出去的字节
  * @retval 同时收到的字节
  * @note   ★TXE和RXNE的配对是SPI最容易错的地方。SPI_I2S_SendData内部就只有一句
  *         SPIx->DR = Data，它【不等待】。必须"等TXE → 写DR → 等RXNE → 读DR"
  *         四步齐全：少等TXE会在上一字节还没发完时覆盖DR（数据丢失）；
  *         少数RXNE会在DR里留下上上次的字节（读到陈旧值，且下一字节的RXNE
  *         判断被带偏）。这里先写后读是标准做法。
  * @note   两个等待都必须带超时。SPE被清掉时TXE的复位值就是1、第一轮就过，
  *         但RXNE永远不来——没有超时就是永久挂死，屏幕冻在最后一帧
  *         （这个外观在AGENTS.md的TIM中断那节已经记录过一次了）。
  */
static uint8_t SD_XferByte(uint8_t Data)
{
	uint32_t Timeout;

	Timeout = SD_TMO_BYTE_CPU;
	while (SPI_I2S_GetFlagStatus(SPI1, SPI_I2S_FLAG_TXE) == RESET)
	{
		if (-- Timeout == 0)
		{
			s_Fatal = 1;
			return 0xFF;
		}
	}
	SPI_I2S_SendData(SPI1, Data);

	Timeout = SD_TMO_BYTE_CPU;
	while (SPI_I2S_GetFlagStatus(SPI1, SPI_I2S_FLAG_RXNE) == RESET)
	{
		if (-- Timeout == 0)
		{
			s_Fatal = 1;
			return 0xFF;
		}
	}
	return (uint8_t)SPI_I2S_ReceiveData(SPI1);
}

/*━━━━━━ 片选（PA4）的写作约定 ━━━━━━
  全文件【写 PA4 电平】的地方一共三处，不要再多出第四处：
      SD_HwInit()     置高            只在初始化时，且必须排在 SD_Select 之前
    ★ SD_Select()     拉低            【全文件唯一一处拉低】
      SD_Deselect()   置高 + 补8个时钟

  自检（两条都要过，数字就是上面那三处数出来的）：
    grep -cP "^\tGPIO_ResetBits\(SD_CS_PORT" lib/Hardware/SD.c   → 必须是 1
    grep -cP "^\tGPIO_SetBits\(SD_CS_PORT"   lib/Hardware/SD.c   → 必须是 2
  ★模式里那个 ^\t 锚定【不能省】：本注释的续行是空格缩进，有了行首制表符
    锚定，注释里的字面串就不会被自己搜到。不加锚定的话计数会各多出 1，
    看起来像是代码里多写了一次片选。这个自匹配的坑我踩过三次——
    每次都想"这次注释里不写出那个字面串就行了"，三次都没做到。

  "命令响应期间CS必须保持低"这条约束靠三层保证：
  ① 命令层函数（SD_SendCmd/SD_WaitReady/SD_ReadData/SD_WriteData）一律不碰CS；
  ② 每个对外操作拆成"公开外壳 + Selected内部函数"，外壳里 Select → Selected
     → Deselect 单出口，"某个early return跳过Deselect"从结构上不可能发生；
  ③ 上面那个"拉低只有一处"的计数自检。

  ⚠⚠ 但【配对正确 ≠ 顺序正确】，而上面三条全都查不出顺序错：
    SD_HwInit() 里那句置高必须排在 SD_Select() 【之前】。曾经为了追求单出口
    把 SD_Select() 提到最前，Select 当场被随后的 HwInit 覆盖，CMD0 就在片选
    无效的状态下发出去——**三条自检全绿、功能全废**，卡永远报 01，
    而且从代码上完全看不出来（Select/Deselect 一个不多一个不少）。
    凡是碰这一段，请把 HwInit / Select / Deselect 三者的先后一起看。*/
static void SD_Select(void)
{
	GPIO_ResetBits(SD_CS_PORT, SD_CS_PIN);
}

/**
  * @brief  释放片选
  * @note   拉高之后【必须】补8个时钟（规范图7-12）。漏了的话当前命令不会失败，
  *         而是【下一条】命令莫名失败——很难联想到片选上去。
  */
static void SD_Deselect(void)
{
	GPIO_SetBits(SD_CS_PORT, SD_CS_PIN);
	SD_XferByte(0xFF);
}

/*==================== 硬件初始化 ====================*/

/**
  * @brief  SPI1的配置（不含协议）
  * @param  Prescaler 分频档，取SD_SLOW_PRESCALER或SD_FAST_PRESCALER
  * @note   ★必须在SPI_Init之后补一句SPI_Cmd(ENABLE)：SPI_Init内部的
  *         CR1_CLEAR_Mask(0x3040)含bit6(SPE)，它会先把SPI关掉。
  *         漏了的症状【不是"没数据"而是"卡死"】：SPE=0时写DR被硬件丢弃、
  *         而TXE的复位值就是1所以轮询秒过、RXNE永远不来。
  * @note   SPI_Mode_Master(0x0104)自带的SSI位(bit8)不是装饰：主模式下
  *         SSM=1时必须SSI=1，否则触发MODF，硬件会自己清掉SPE和MSTR。
  *         别手写0x0004。
  */
static void SD_SpiConfig(uint16_t Prescaler)
{
	SPI_InitTypeDef SPI_InitStructure;

	SPI_InitStructure.SPI_Direction         = SPI_Direction_2Lines_FullDuplex;
	SPI_InitStructure.SPI_Mode              = SPI_Mode_Master;
	SPI_InitStructure.SPI_DataSize          = SPI_DataSize_8b;
	SPI_InitStructure.SPI_CPOL              = SPI_CPOL_Low;
	SPI_InitStructure.SPI_CPHA              = SPI_CPHA_1Edge;
	SPI_InitStructure.SPI_NSS               = SPI_NSS_Soft;
	SPI_InitStructure.SPI_BaudRatePrescaler = Prescaler;
	SPI_InitStructure.SPI_FirstBit          = SPI_FirstBit_MSB;
	SPI_InitStructure.SPI_CRCPolynomial     = 7;	//SD不走SPI硬件CRC，占位
	SPI_Init(SPI1, &SPI_InitStructure);

	SPI_Cmd(SPI1, ENABLE);					//★见上面@note，漏了会卡死
}

/**
  * @brief  时钟 + GPIO + SPI，全部按慢档配好，片选置高
  * @note   顺序很关键：★CS必须先配成推挽输出【并置高】，再去配SPI。
  *         反过来的话，SPI使能的瞬间片选是浮空的，卡可能被误选中。
  * @note   MISO用IPU不是AF_PP：AF_PP会让MCU去驱动MISO，和卡抢同一根线。
  */
static void SD_HwInit(void)
{
	GPIO_InitTypeDef GPIO_InitStructure;

	/*开启时钟。SPI1挂APB2（72MHz），不是APB1*/
	RCC_APB2PeriphClockCmd(RCC_APB2Periph_GPIOA, ENABLE);	//开启GPIOA的时钟
	RCC_APB2PeriphClockCmd(RCC_APB2Periph_SPI1, ENABLE);	//开启SPI1的时钟（挂APB2）

	/*片选先落地。逐引脚调用，不用合并掩码——GPIO_Init会重写掩码里每一位的
	  CNF/MODE，一个手滑写成GPIO_Pin_All就会毁掉PA0/PA1(ADC)、PA9(按键2)、
	  甚至PA13/PA14(SWD)*/
	GPIO_InitStructure.GPIO_Mode  = GPIO_Mode_Out_PP;
	GPIO_InitStructure.GPIO_Pin   = SD_CS_PIN;
	GPIO_InitStructure.GPIO_Speed = GPIO_Speed_50MHz;
	GPIO_Init(SD_CS_PORT, &GPIO_InitStructure);
	GPIO_SetBits(SD_CS_PORT, SD_CS_PIN);					//片选无效（高）

	/*SCK和MOSI是复用推挽，MISO是上拉输入*/
	GPIO_InitStructure.GPIO_Mode  = GPIO_Mode_AF_PP;
	GPIO_InitStructure.GPIO_Pin   = GPIO_Pin_5 | GPIO_Pin_7;
	GPIO_Init(GPIOA, &GPIO_InitStructure);

	GPIO_InitStructure.GPIO_Mode  = GPIO_Mode_IPU;
	GPIO_InitStructure.GPIO_Pin   = GPIO_Pin_6;
	GPIO_Init(GPIOA, &GPIO_InitStructure);

	SD_SpiConfig(SD_SLOW_PRESCALER);
}

/*==================== 命令层 ====================*/
/*本层所有函数都【不碰CS】——理由见SD_Select上面的说明。*/

/**
  * @brief  发一条命令并等R1响应
  * @param  Cmd 命令号，0~63（不含0x40起始位）
  * @param  Arg 32位参数
  * @param  Crc 校验字节。只有CMD0/CMD8需要真CRC，其余发0x01占位
  * @retval 卡返回的R1字节。0xFF=超时（没卡或卡没应答）
  * @note   ★等R1的判据是"最高位为0"，【不是】"不等于0xFF"：卡在内部忙的时候
  *         会先吐若干个0x00，用 !=0xFF 会把这些0x00误判成有效响应。
  *         规范里这个窗口叫NCR，上限8字节时间。
  */
static uint8_t SD_SendCmd(uint8_t Cmd, uint32_t Arg, uint8_t Crc)
{
	uint32_t Timeout;
	uint8_t  r;

	SD_XferByte(0xFF);						//片选拉低后的第一个字节是余量

	SD_XferByte((uint8_t)(0x40 | Cmd));
	SD_XferByte((uint8_t)(Arg >> 24));
	SD_XferByte((uint8_t)(Arg >> 16));
	SD_XferByte((uint8_t)(Arg >> 8));
	SD_XferByte((uint8_t)Arg);
	SD_XferByte(Crc);

	Timeout = SD_TMO_R1;
	do
	{
		r = SD_XferByte(0xFF);
		if (s_Fatal) return 0xFF;
	} while ((r & 0x80) && -- Timeout);

	return r;
}

/**
  * @brief  等卡释放忙状态（数据线回到高电平）
  * @retval 0=空闲，非0=超时
  */
static uint8_t SD_WaitReady(void)
{
	uint32_t Timeout = SD_TMO_BUSY;
	uint8_t  r;

	do
	{
		r = SD_XferByte(0xFF);

		/*★s_Fatal必须在拿回r之后【立刻】查，不能放在循环体末尾靠while条件兜。
		  因为SPI层卡死时SD_XferByte返回的正好是0xFF——而0xFF恰好是本函数
		  "卡已空闲"的退出条件。不在这里拦，一条死掉的SPI会被判成"卡已就绪"，
		  故障换个码从别处浮上来，把人引向卡和接线去查*/
		if (s_Fatal)   return 1;
		if (r == 0xFF) return 0;			//卡空闲
	} while (-- Timeout);

	return 1;								//超时
}

/**
  * @brief  等数据令牌，然后收一个数据块
  * @param  Buf 接收缓冲
  * @param  Len 收多少字节（块是512，CSD是16）
  * @retval SD_Status
  * @note   ★CMD9(读CSD)走的是【和CMD17同一条路径】：响应都是
  *         "R1 + 0xFE数据令牌 + 数据 + 2字节CRC"，不是"R1后面直接跟16字节"。
  *         把CMD9当成裸16字节收，是这类驱动最常见的错。
  * @note   ★这里的判据和上面等R1的【故意不同】：令牌就是要"恰好等于0xFE"，
  *         用最高位判会在第一个0xFF就退出，慢卡必然被误报超时。
  *         别把SD_SendCmd里那段复制过来。
  */
static SD_Status SD_ReadData(uint8_t *Buf, uint32_t Len)
{
	uint32_t Timeout, i;
	uint8_t  r;

	Timeout = SD_TMO_TOKEN;
	do
	{
		r = SD_XferByte(0xFF);
		if (s_Fatal) return SD_ERR_FATAL;
	} while (r != 0xFE && -- Timeout);
	if (r != 0xFE) return SD_ERR_TOKEN;

	for (i = 0; i < Len; i ++)
	{
		Buf[i] = SD_XferByte(0xFF);
		/*★每个字节都要回查一次s_Fatal：SPI层卡死时SD_XferByte每次都把整个
		  超时预算烧完，512字节就是512倍——主循环会卡几秒，
		  不但心跳停住，Key的16槽队列也会溢出。这里break掉才对得起
		  "粘滞标志让整条链立刻返回"这个设计*/
		if (s_Fatal) return SD_ERR_FATAL;
	}
	SD_XferByte(0xFF);						//2字节CRC，SPI模式下卡不校验，丢掉
	SD_XferByte(0xFF);

	return s_Fatal ? SD_ERR_FATAL : SD_OK;
}

/**
  * @brief  发数据令牌和512字节数据，并检查数据响应
  * @param  Buf 要写的数据
  * @param  Token 数据令牌，单块写是0xFE
  * @retval SD_Status
  * @note   ★数据响应【必须】检查：卡收到数据后回一个字节，低5位0b00101才算
  *         接受，其余值都是拒收（CRC错、写保护、越界等）。漏了的话，
  *         "卡拒收"会被当成"写成功"——而且是静默的。
  */
static SD_Status SD_WriteData(const uint8_t *Buf, uint8_t Token)
{
	uint32_t i;
	uint8_t  Resp;

	SD_XferByte(Token);
	for (i = 0; i < 512; i ++)
	{
		SD_XferByte(Buf[i]);
		if (s_Fatal) return SD_ERR_FATAL;	//理由同SD_ReadData，别省这一步
	}
	SD_XferByte(0xFF);						//2字节CRC占位（没发过CMD59开CRC校验收）
	SD_XferByte(0xFF);

	if (s_Fatal) return SD_ERR_FATAL;

	Resp = SD_XferByte(0xFF);
	if ((Resp & 0x1F) != 0x05)
	{
		return SD_ERR_DATA_RESP;
	}
	return SD_OK;
}

/**
  * @brief  块号 → 命令参数
  * @param  Lba 块号
  * @note   ★全工程唯一的地址换算点，别在别处再乘512。
  * @note   判据是CMD58读到的CCS位（当前寻址模式），不是CSD的版本号：
  *         卡是SDHC/SDXC时地址本来就是块号；是SDSC时地址是字节偏移，要乘512。
  */
static uint32_t SD_MakeArg(uint32_t Lba)
{
	return SD_Info.Ccs ? Lba : (Lba << 9);
}

/*==================== 块读写 ====================*/

/**
  * @brief  读一个块（内部版，调用者负责片选）
  */
static SD_Status SD_ReadBlockSelected(uint32_t Lba, uint8_t *Buf)
{
	uint8_t r;

	r = SD_SendCmd(17, SD_MakeArg(Lba), 0x01);
	if (r != 0x00)
	{
		return (r == 0xFF) ? SD_ERR_TIMEOUT : SD_ERR_R1;
	}

	return SD_ReadData(Buf, 512);
}

/**
  * @brief  写一个块（内部版，调用者负责片选）
  * @note   写流程里有两步最容易漏，漏了都是静默出错：
  *         ① 发CMD24【之前】先等卡空闲——卡可能还在处理上一次写。
  *            漏了的症状是"偶尔写失败"，而且换张快卡就复现不了。
  *         ② 数据响应之后【必须先等忙释放】再发CMD13。漏了的话下一条命令
  *            会撞在卡的编程周期里。
  */
static SD_Status SD_WriteBlockSelected(uint32_t Lba, const uint8_t *Buf)
{
	SD_Status st;
	uint8_t r;

	if (SD_WaitReady()) return SD_ERR_BUSY;			//①

	r = SD_SendCmd(24, SD_MakeArg(Lba), 0x01);
	if (r != 0x00)
	{
		return (r == 0xFF) ? SD_ERR_TIMEOUT : SD_ERR_R1;
	}

	st = SD_WriteData(Buf, 0xFE);
	if (st != SD_OK) return st;

	if (SD_WaitReady()) return SD_ERR_BUSY;			//②

	/*CMD13查状态。它回的是R2（两个字节）。
	  ★目前只检查第一个字节（R1）——为0只说明命令本身没被拒。
	  第二个字节才是"卡内部编程有没有失败"（写保护、ECC失败、CC错误…），
	  但那些位的含义要对着SD规范逐位核实，没核实之前不猜：猜错会让一张健康卡
	  被误报成写失败，比漏报更坏。
	  代价是 SD_ERR_WRITE_PROTECT(10) 和 SD_ERR_CRC(12) 现在取不到，
	  SD.h里已标注为保留。真正兜住"写没落盘"的是 SD_WriteTest 的读回比对。
	  下一轮做FatFs时把规范找齐，把这两个字节一起解析*/
	r = SD_SendCmd(13, 0, 0x01);
	(void)SD_XferByte(0xFF);
	if (r != 0x00) return SD_ERR_R1;

	return SD_OK;
}

/**
  * @brief  读一个512字节块
  * @param  Lba 块号（不是字节地址）
  * @param  Buf 接收缓冲，调用者保证有512字节
  * @retval SD_Status
  */
SD_Status SD_ReadBlock(uint32_t Lba, uint8_t *Buf)
{
	SD_Status st;

	if (s_Fatal)											st = SD_ERR_FATAL;
	else if (!SD_Info.Ready)								st = SD_ERR_STATE;
	else if (SD_Info.SectorCount && Lba >= SD_Info.SectorCount)	st = SD_ERR_PARM;
	else
	{
		SD_Select();
		st = SD_ReadBlockSelected(Lba, Buf);
		SD_Deselect();
	}

	/*失败要落到LastError上，否则屏幕第4行永远是00，故障码形同虚设*/
	if (st != SD_OK) SD_Info.LastError = (uint8_t)st;
	return st;
}

/**
  * @brief  写一个512字节块
  * @param  Lba 块号（不是字节地址）
  * @param  Buf 要写的数据
  * @retval SD_Status
  */
SD_Status SD_WriteBlock(uint32_t Lba, const uint8_t *Buf)
{
	SD_Status st;

	if (s_Fatal)											st = SD_ERR_FATAL;
	else if (!SD_Info.Ready)								st = SD_ERR_STATE;
	else if (SD_Info.SectorCount && Lba >= SD_Info.SectorCount)	st = SD_ERR_PARM;
	else
	{
		SD_Select();
		st = SD_WriteBlockSelected(Lba, Buf);
		SD_Deselect();
	}

	if (st != SD_OK) SD_Info.LastError = (uint8_t)st;
	return st;
}

/*==================== CSD与初始化 ====================*/

/**
  * @brief  从16字节CSD里算出扇区总数
  * @retval SD_Status
  * @note   ★版本看的是【高2位】Csd[0]>>6，不是低2位。写反会得到一个
  *         "看起来很正常"的荒谬容量，比显示UNKNOWN更难发现。
  * @note   两道防呆各一行，少了都会静默出错：
  *         ① v1的移位量由 RBL和MULT 决定，值域失控时移位结果没有意义；
  *         ② SDXC的C_SIZE最大22位，不夹取的话 (C_SIZE+1)*1024 在
  *            uint32上虽然不溢出，但2TB以上的卡算出来会偏。
  * @note   自检基准值：8GB SDHC → C_SIZE≈15349 → 7675MiB；
  *         2GB SDSC → C_SIZE=4095, MULT=7, RBL=10 → 4194304扇区 = 2GiB。
  */
static SD_Status SD_ParseCsd(void)
{
	uint32_t C_SIZE, MULT, Sectors, RBL;
	uint8_t  Ver;

	Ver = (uint8_t)(s_Csd[0] >> 6);			//CSD_STRUCTURE在bit127:126
	SD_Info.CsdVer = Ver;

	if (Ver == 1)							//v2：SDHC / SDXC
	{
		C_SIZE = ((uint32_t)(s_Csd[7] & 0x3F) << 16) | ((uint32_t)s_Csd[8] << 8) | s_Csd[9];
		/*★C_SIZE 必须夹。C_SIZE=0x3FFFFF 时 (C_SIZE+1)*1024 = 2^32，
		  在uint32上【正好回绕成0】：容量显示UNKNOWN，更糟的是SectorCount=0
		  被当成"容量未知"，SD_ReadBlock/SD_WriteBlock里的越界保护会被短路掉，
		  那张卡上任何LBA都不再被拦。规范中SDXC的C_SIZE上限是0x3FFEFF，
		  0x3FFFFF是未分配值（真卡不会有），直接夹掉*/
		if (C_SIZE > 0x3FFEFFUL) C_SIZE = 0x3FFEFFUL;
		Sectors = (C_SIZE + 1) * 1024;		//容量字节 = (C_SIZE+1) × 512KB
	}
	else if (Ver == 0)						//v1：SDSC
	{
		RBL    = s_Csd[5] & 0x0F;
		C_SIZE = ((uint32_t)(s_Csd[6] & 0x03) << 10) | ((uint32_t)s_Csd[7] << 2) |
		         ((uint32_t)s_Csd[8] >> 6);
		MULT   = ((uint32_t)(s_Csd[9] & 0x03) << 1) | ((uint32_t)s_Csd[10] >> 7);

		if (RBL < 9 || RBL > 11 || MULT > 7) return SD_ERR_CSD;		//防呆①
		Sectors = ((uint32_t)C_SIZE + 1) << (MULT + RBL - 7);		//字节数/512
	}
	else
	{
		return SD_ERR_CSD;
	}

	/*CSD的版本必须和CMD58读到的CCS一致。不一致说明两次读里至少有一次是错的，
	  与其带着错误的地址模式往下跑，不如在这里拦住*/
	if ((Ver == 1) != (SD_Info.Ccs == 1)) return SD_ERR_CSD;

	SD_Info.SectorCount = Sectors;
	SD_Info.CapacityMB  = Sectors >> 11;	//1MiB = 2048扇区

	/*★卡型只能靠【容量】分，不能只看CSD版本和CCS——SDHC和SDXC这两项完全相同
	  （都是CSD v2 + CCS=1），没有任何一位能区分它们。所以SD_TYPE_SDXC必须在这里
	  判，否则那条分支永远取不到，32GB以上的卡会一律显示成SDHC*/
	if (SD_Info.Ccs == 0)
	{
		SD_Info.Type = SD_TYPE_SDSC;						//字节寻址
	}
	else if (Sectors > 67108864UL)						//>32GiB（=32*1024^3/512扇区）
	{
		/*★这里【必须】用常量，不能写成 32UL*1024UL*1024UL*1024UL/512UL：
		  那是 2^35，32位的 unsigned long 上会回绕成 8，除以512得 0，
		  于是"大于0"对任何卡都成立——所有卡都会被判成SDXC*/
		SD_Info.Type = SD_TYPE_SDXC;
	}
	else
	{
		SD_Info.Type = SD_TYPE_SDHC;
	}
	return SD_OK;
}

/**
  * @brief  识别流程的执行体【片选由调用者负责，本函数不碰CS】
  * @note   识别全程片选必须一直保持低，中间不能被别的函数拉高。这里刻意不写
  *         SD_Select/SD_Deselect——本函数有一堆提前return，如果每处都手写一句
  *         收尾，迟早会漏一个，而漏了的后果是片选留在低电平、【下一条】命令
  *         莫名失败。交给外层单出口去配对。
  */
static SD_Status SD_InitSelected(void)
{
	uint8_t  r, i;
	uint8_t  Ocr[4];
	SD_Status st;
	uint32_t Arg;

	/*清状态。没读成的东西一律留0xFF——这样屏幕上"B0:FF FF FF FF"的含义
	  唯一确定是"这次根本没读成"，而不是"读到了FF"*/
	SD_Info.Ready       = 0;
	SD_Info.Type        = SD_TYPE_NONE;
	SD_Info.Ccs         = 0;
	SD_Info.CsdVer      = 255;
	SD_Info.SectorCount = 0;
	SD_Info.CapacityMB  = 0;
	for (i = 0; i < 4; i ++) SD_Info.Blk0Head[i] = 0xFF;
	for (i = 0; i < 2; i ++) SD_Info.Blk0Tail[i] = 0xFF;
	/*★TestResult也要清。不清的话换一张好卡重新识别之后，行4还挂着上一张卡的
	  "RW:FAIL 15"，操作者会去追一个根本没重测过的写故障。
	  以后往SD_InfoDef里加字段，记得这里同步加一行*/
	SD_Info.TestResult  = SD_TEST_NONE;
	SD_Info.TestLba     = 0;
	s_Fatal = 0;
	s_IsV1  = 0;

	/*注意：GPIO/SPI配置和74个上电时钟都不在这里——它们必须在【片选拉低之前】做完，
	  所以放在外层 SD_InitInternal 里。本函数进来时片选已经是低的了*/

	/*CMD0：必须回0x01（进入idle态）。回0xFF说明卡根本没应答*/
	r = SD_SendCmd(0, 0, 0x95);
	if (r != 0x01)
	{
		return SD_ERR_NO_CARD;
	}

	/*CMD8：只有SD v2.0以上支持，卡会回4字节的R7把参数回显回来。
	  ★CMD8报非法命令(bit2)【不是错误】，而是"这是一张SD v1.x卡"，不能abort——
	  否则所有≤2GB的卡直接用不了。v1卡后面的HCS位要送0，地址走字节偏移。
	  ★而且这种情况【不要再读那4字节回显】：只有R1没报非法命令时卡才会发。*/
	r = SD_SendCmd(8, 0x000001AA, 0x87);
	if (r == 0x01)
	{
		for (i = 0; i < 4; i ++) Ocr[i] = SD_XferByte(0xFF);
		if (Ocr[2] != 0x01 || Ocr[3] != 0xAA)		//回显的电压和校验图样
		{
			return SD_ERR_CMD8;
		}
	}
	else if (r != 0xFF && (r & 0x04))
	{
		/*r==0xFF是"没应答"，它同样满足 r & 0x04，所以必须先排除掉——
		  否则一次接触不良的CMD8会被当成"这是张SD v1.x卡"，
		  接着ACMD41带着HCS=0重试100次（v2卡永远不接受），
		  最后报03"卡内部初始化失败"，把人引去查一张其实没问题的卡*/
		s_IsV1 = 1;									//SD v1.x卡
	}
	else
	{
		return (r == 0xFF) ? SD_ERR_NO_CARD : SD_ERR_CMD8;
	}

	/*ACMD41：循环等卡把idle位清掉。
	  ★两个经典坑：① 每次重试都要【重新发CMD55】——app标志是一次性的，
	    在循环外只发一次是最常见的写法错误；② 重试之间必须有真实延时，
	    否则100次在几十毫秒内打完，卡还没上电完，必然误判失败。*/
	r = 0xFF;
	for (i = 0; i < SD_ACMD41_RETRY; i ++)
	{
		r = SD_SendCmd(55, 0, 0x01);
		if (r > 0x01) break;						//CMD55本身失败
		Arg = s_IsV1 ? 0x00000000 : 0x40000000;		//HCS位只对v2卡有意义
		r = SD_SendCmd(41, Arg, 0x01);
		if (r == 0x00) break;
		Delay_ms(10);
	}
	if (r != 0x00)
	{
		return SD_ERR_NOT_READY;
	}

	/*CMD58：读OCR。Ocr[0]=bit31:24，Ocr[1]=23:16，Ocr[2]=15:8，Ocr[3]=7:0。
	  bit31是上电完成位，**bit30是CCS**（在Ocr[0]的bit6）。
	  ★CCS决定了后面所有地址是块号还是字节偏移，取错了整个驱动都错——
	  但表面现象只是"容量显示成 SDSC"。*/
	r = SD_SendCmd(58, 0, 0x01);
	if (r != 0x00)
	{
		if (!s_IsV1)								//v1卡不支持CMD58时可以继续，按字节寻址
		{
			return (r == 0xFF) ? SD_ERR_TIMEOUT : SD_ERR_R1;
		}
	}
	else
	{
		for (i = 0; i < 4; i ++) Ocr[i] = SD_XferByte(0xFF);
		/*电压窗口是OCR的 bit23:15（2.7~3.6V阶梯），即 Ocr[1] 的整个字节
		  加上 Ocr[2] 的最高位。★3.3V 落在 bit17/bit18（3.3~3.4 / 3.2~3.3），
		  不是 bit23——之前这里只查了 bit23（2.7~2.8V那一格），
		  注释却写着"2.7~3.6V窗口"，代码和注释对不上*/
		if ((Ocr[1] & 0xFF) == 0 && (Ocr[2] & 0x80) == 0)
		{
			return SD_ERR_VOLTAGE;
		}
		SD_Info.Ccs = (Ocr[0] & 0x40) ? 1 : 0;
	}

	/*提速。★时机是CMD58之后、CMD9之前，【不能】放在ACMD41循环中途：
	  那会让"某次重试正好撞上卡内部状态切换"变成偶发失败。*/
	SD_SpiConfig(SD_FAST_PRESCALER);

	/*CMD9读CSD。容量要靠它，但它读不出来【不影响读写】——
	  寻址模式由CCS决定，和CSD解析成功与否是解耦的*/
	r = SD_SendCmd(9, 0, 0x01);
	if (r == 0x00)
	{
		if (SD_ReadData(s_Csd, 16) == SD_OK)
		{
			st = SD_ParseCsd();
			if (st != SD_OK) SD_Info.LastError = (uint8_t)st;
		}
	}

	/*块0：只读一次缓存进SD_Info，界面只做格式化。绝不能每帧重读卡——那是阻塞的。
	  这块读不成【不算致命】（卡可能是空的/未格式化的），但错误码必须记下来：
	  否则屏幕上会同时出现 "RW:---- 00"（看着像成功）和 "SIG:FFFF NG"，
	  自相矛盾而且没有线索——行4那个码存在的意义正是区分这两种情况*/
	st = SD_ReadBlockSelected(0, s_Work);
	if (st == SD_OK)
	{
		for (i = 0; i < 4; i ++) SD_Info.Blk0Head[i] = s_Work[i];
		SD_Info.Blk0Tail[0] = s_Work[510];
		SD_Info.Blk0Tail[1] = s_Work[511];
	}
	else
	{
		SD_Info.LastError = (uint8_t)st;
	}

	SD_Info.Ready = 1;
	return SD_OK;
}

/**
  * @brief  识别流程的外壳：片选的单出口配对
  * @note   执行体里有一堆提前return，这里统一收尾。Deselect只有这一个出口，
  *         所以"某个return忘了释放片选"从结构上不可能发生。
  */
static SD_Status SD_InitInternal(void)
{
	SD_Status st;
	uint8_t i;

	/*★这里三样东西的先后【不能调换】：
	  ① SD_HwInit() 配GPIO/SPI——它内部会把片选【置高】；
	  ② 74个上电时钟必须在【片选高】的状态下发（规范要求）；
	  ③ 之后才 SD_Select() 拉低片选，再去发 CMD0。
	  曾经把 SD_Select() 提到最前面，结果被 SD_HwInit() 又拉了回去，
	  CMD0 就在片选无效的状态下发出去，卡根本没被选中，现象是永远报"没卡"(01)。
	  ★这个错误从代码上【看不出来】：Select/Deselect 的配对完全正确，
	    错的只是它们和 HwInit 之间的先后。改动这一段时请把三者的顺序一起看。*/
	SD_HwInit();

	for (i = 0; i < 10; i ++)
	{
		SD_XferByte(0xFF);					//10字节 = 80个时钟，MOSI保持高
	}
	Delay_ms(1);

	SD_Select();
	st = SD_InitSelected();
	SD_Deselect();
	return st;
}

/**
  * @brief  SD卡初始化
  * @retval SD_Status
  * @note   幂等，可以反复调（拔卡重插后按一下键重跑即可，不用断电）。
  *         每次进来先把状态清干净，否则拔卡之后屏幕上还显示着上次的卡信息。
  * @note   ★内部有阻塞延时（ACMD41的重试间隔，最坏约1.1s），
  *         只能在主循环上下文调用。没插卡时约100ms就返回（CMD0就超时了）。
  */
SD_Status SD_Init(void)
{
	SD_Status st = SD_InitInternal();

	if (st != SD_OK) SD_Info.LastError = (uint8_t)st;
	return st;
}

/**
  * @brief  回环自检（不需要SD卡）
  * @retval 0=通过，非0=第一次对不上的差异值
  * @note   用法见SD.h。它会自己把SPI配好，不需要先调SD_Init。
  * @note   整个过程中片选保持无效（高），这样即使插着卡也不会被选中。
  */
uint8_t SD_LoopbackTest(void)
{
	uint8_t i, Sent, Got;

	s_Fatal = 0;
	SD_HwInit();							//片选高，慢档

	for (i = 0; i < 8; i ++)
	{
		Sent = (uint8_t)(0xA5 ^ (i * 0x11));
		Got  = SD_XferByte(Sent);
		if (Got != Sent) return (uint8_t)(Got ^ Sent);
	}

	return (s_Fatal ? 0xFF : 0x00);
}

/**
  * @brief  写测试的执行体。用early return而不是goto，失败点直接返回
  */
static SD_Status SD_WriteTestRun(uint32_t Lba)
{
	uint32_t i;
	SD_Status stVerify, stRestore, st;

	/*① 存档末块原内容*/
	st = SD_ReadBlock(Lba, s_Work);
	if (st != SD_OK) return st;

	/*② 写变值图案。变值是为了堵住这个测试唯一会骗人的地方：全0x00/全0xFF
	  在"写根本没生效"时也会比对通过（擦除态块本来就是全FF）*/
	for (i = 0; i < 512; i ++)
	{
		s_Verify[i] = (uint8_t)(i ^ 0xA5);
	}
	st = SD_WriteBlock(Lba, s_Verify);
	if (st != SD_OK) return st;			//图案没写进去，块里还是原内容，不用还原

	/*★从这一行往后，卡上的末块已经是测试图案了——【无论后面发生什么，
	  都必须走到第④步把它还原回去】。所以从这里开始不再用 early return：
	  各步结果先记下来，最后统一决定返回值。

	  这是本函数最容易写错的地方，而且错法很隐蔽：读回比对一失败就 return 的话，
	  原内容永远留在 s_Work 里没人用，卡上那个块被图案覆盖着——
	  那是【真的丢数据】，而且下次再跑这个测试会拿图案当"原内容"存档，
	  把损失钉死。还原失败（第⑤步）是本测试最严重的故障，所以它的错优先报。*/
	stVerify = SD_ReadBlock(Lba, s_Verify);
	if (stVerify == SD_OK)
	{
		for (i = 0; i < 512; i ++)
		{
			if (s_Verify[i] != (uint8_t)(i ^ 0xA5))
			{
				stVerify = SD_ERR_DATA_RESP;
				break;
			}
		}
	}

	/*④ 还原。无条件执行*/
	stRestore = SD_WriteBlock(Lba, s_Work);

	/*⑤ 再读确认还原成功*/
	if (stRestore == SD_OK)
	{
		stRestore = SD_ReadBlock(Lba, s_Verify);
		if (stRestore == SD_OK)
		{
			for (i = 0; i < 512; i ++)
			{
				if (s_Verify[i] != s_Work[i])
				{
					stRestore = SD_ERR_DATA_RESP;
					break;
				}
			}
		}
	}

	if (stRestore != SD_OK) return stRestore;	//卡上有残留，优先报这个
	return stVerify;							//还原没问题，再报比对结果
}

/**
  * @brief  写-读回-还原自检
  * @retval SD_Status
  * @note   只碰卡的【最后一个块】。FAT是从前往后分配的，末块几乎不可能被占用，
  *         但这是【概率不是保证】——卡接近写满或高度碎片化时末块可能有数据，
  *         这个测试会短暂覆盖它（正常情况下会还原回去）。
  *         ⚠ 绝不允许把测试块改成块0或随便一块，那会直接毁掉分区表。
  * @note   容量未知时直接拒绝：不知道哪个是末块，就不能安全写任何地方。
  */
SD_Status SD_WriteTest(void)
{
	SD_Status st;

	if (s_Fatal)
	{
		/*★这里【不要】写LastError：13（SPI层卡死）已经由failed的读/写记下了，
		  在这里覆盖成15会把根因擦掉，屏幕上就变成"卡没初始化"，
		  而实际上卡是好的、坏的是MCU这一侧*/
		SD_Info.TestResult = SD_TEST_FAIL;
		return SD_ERR_FATAL;
	}
	if (!SD_Info.Ready)
	{
		SD_Info.TestResult = SD_TEST_FAIL;
		SD_Info.LastError  = SD_ERR_STATE;
		return SD_ERR_STATE;
	}
	if (SD_Info.SectorCount == 0)
	{
		SD_Info.TestResult = SD_TEST_FAIL;
		SD_Info.LastError  = SD_ERR_CSD;
		return SD_ERR_CSD;
	}

	SD_Info.TestLba    = SD_Info.SectorCount - 1;
	SD_Info.TestResult = SD_TEST_RUN;

	st = SD_WriteTestRun(SD_Info.TestLba);

	SD_Info.TestResult = (st == SD_OK) ? SD_TEST_OK : SD_TEST_FAIL;
	if (st != SD_OK) SD_Info.LastError = (uint8_t)st;

	return st;
}
