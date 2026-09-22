#ifndef __SD_H
#define __SD_H

#include "stm32f10x.h"                  // Device header

/*---------------- 卡类型 ----------------*/
#define SD_TYPE_NONE		0
#define SD_TYPE_SDSC		1		//≤2GB，地址是【字节偏移】，要左移9位
#define SD_TYPE_SDHC		2		//2GB~32GB，地址是【块号】
#define SD_TYPE_SDXC		3		//32GB~2TB，地址是【块号】

/*---------------- 写测试结果 ----------------*/
#define SD_TEST_NONE		0
#define SD_TEST_RUN			1
#define SD_TEST_OK			2
#define SD_TEST_FAIL		3

/*---------------- 返回码 ----------------
  0=成功。非0的码值就是"走到第几步"的进度指示器，会原样显示在OLED第4行，
  所以【没有调试器也能读故障】：
    01 没插卡 / 接线错 / 模块没供电        07 读失败（等不到数据令牌）
    02 卡型不支持（CMD8回显不对）          08 写失败（卡拒收）
    03 卡内部初始化失败（ACMD41熬满1秒）   09 写失败（忙不释放）
    04 卡的电压窗口不含3.3V                11 CSD读不出 / 版本和CCS矛盾
    05 命令返回了错误位                    13 SPI层卡死（多半是漏了SPI_Cmd(ENABLE)）
    06 命令超时                            14 块号越界
                                          15 卡没初始化就调用读写*/
typedef enum
{
	SD_OK = 0,

	/*1~4：初始化前半段*/
	SD_ERR_NO_CARD   = 1,		//CMD0无响应
	SD_ERR_CMD8      = 2,		//CMD8回显不是0x1AA
	SD_ERR_NOT_READY = 3,		//ACMD41熬满
	SD_ERR_VOLTAGE   = 4,		//OCR电压窗口不含3.3V

	/*5~6：命令层*/
	SD_ERR_R1        = 5,
	SD_ERR_TIMEOUT   = 6,

	/*7~10：数据层*/
	SD_ERR_TOKEN     = 7,		//等不到0xFE数据令牌
	SD_ERR_DATA_RESP = 8,		//写响应不是0b00101
	SD_ERR_BUSY      = 9,		//写忙不释放

	/*★下面这两个码当前【没有任何代码会返回】，排查时别指着它们等。
	  10=写保护、12=CRC错，本该由CMD13的R2状态字节报出来，但R2第二个字节的
	  位含义要对着SD规范逐位核，没核实之前不猜——猜错会让健康卡被误报写失败。
	  现在只检查R2的第一个字节（R1）。
	  实际兜底：SD_WriteTest的"读回比对"那一步能抓住"写没落盘"的卡。
	  等下一轮做FatFs（写的正确性最要紧）时连规范一起补*/
	SD_ERR_WRITE_PROTECT = 10,	//保留，暂不返回
	SD_ERR_CSD       = 11,
	SD_ERR_CRC       = 12,		//保留，暂不返回
	SD_ERR_FATAL     = 13,		//SPI层卡死
	SD_ERR_PARM      = 14,
	SD_ERR_STATE     = 15,		//卡没初始化
} SD_Status;

/*卡信息。对上层只读，别在外部改。
  【不需要volatile】—— 别照抄AD_Value和Key队列那两个：写它的只有SD_Init和
  SD_WriteTest，读者是主循环里的画屏函数，两者是同一个上下文，不存在并发。*/
typedef struct
{
	uint8_t  Ready;				//1=已进入传输态，可以读写
	uint8_t  Type;				//SD_TYPE_xxx
	uint8_t  Ccs;				//CMD58读到的CCS原值。1=块地址，0=字节地址
	uint8_t  CsdVer;			//CSD结构版本，255=没读到
	uint32_t SectorCount;		//0=未知
	uint32_t CapacityMB;		//0=未知
	uint8_t  Blk0Head[4];		//块0偏移0~3。没读成时保持0xFF
	uint8_t  Blk0Tail[2];		//块0偏移510/511。没读成时保持0xFF
	uint8_t  TestResult;		//SD_TEST_xxx
	uint32_t TestLba;			//写测试实际用的块号
	uint8_t  LastError;			//最近一次【失败】的码，只被新的失败覆盖，成功不清零
} SD_InfoDef;

extern SD_InfoDef SD_Info;

/*初始化：配SPI1 + 卡上电时序 + 识别 + 读CSD + 缓存块0。幂等，可以反复调
  （拔卡后重新插上，按一下键重跑本函数就行，不用断电）。
  ★内部有阻塞延时（ACMD41的重试间隔），只能在主循环上下文调用，别在中断里调*/
SD_Status SD_Init(void);

/*读/写一个512字节块。Lba是块号，不是字节地址。
  块号→命令参数 的换算统一在SD.c的SD_MakeArg里做，调用者不要自己乘512*/
SD_Status SD_ReadBlock(uint32_t Lba, uint8_t *Buf);
SD_Status SD_WriteBlock(uint32_t Lba, const uint8_t *Buf);

/*写-读回-还原自检。只碰卡的【最后一个块】，容量未知时直接拒绝。
  ⚠ 这条路径【无法自检】：SDSC卡上如果漏了<<9换算，块0在两种模式下地址都是0、
  末块错换算后仍落在容量内、写回读比照样通过。只有拿一张2GB以下的卡才能验到
  它；SDHC卡根本不走这条路。同理，"ACMD41重试间隔漏了延时"只在个别慢卡上偶发，
  "忙等待超时设小了"只在老化卡上偶发，"数据响应令牌没检查"只在卡真的拒收时才暴露。
  这四条上板验证发现不了，只能靠代码走查。*/

/*完整自检：读末块存档 → 写变值图案 → 读回比对 → 写回原内容 → 再读确认还原*/
SD_Status SD_WriteTest(void);

/*回环自检，【不需要SD卡】：拔掉模块，用杜邦线短接 PA6(MISO) 和 PA7(MOSI)，
  然后调它。返回0=通过。MOSI发什么MISO就该收到什么。
  ★这一步把"GPIO模式/分频/CPOL/CPHA/TXE-RXNE配对"整层在没有卡的情况下
  一次性证明掉。跳过它，后面每个故障都要在"是SPI层还是卡层"之间两头猜。
  拔掉短接线后必须返回非0——那是反向确认，防止"恒返回0"的假通过。*/
uint8_t SD_LoopbackTest(void);

/*---------------- 外设与引脚占用提醒 ----------------
  SPI1 走【默认映射】。全工程不调用任何 GPIO_PinRemapConfig —— 这点是刻意的：
  Key_Init() 会清掉 AFIO->MAPR 低24位（抹掉所有重映射），默认映射天然躲开。

  PA4 = SPI1_NSS，本模块当【普通推挽输出】手动片选，不用 SPI_SSOutputCmd
        （那是 NSS_Hard 才用的，本模块是 NSS_Soft）
  PA5 = SPI1_SCK   AF_PP
  PA6 = SPI1_MISO  IPU（不是 AF_PP：AF_PP 会让 MCU 去驱动 MISO 和卡打架）
  PA7 = SPI1_MOSI  AF_PP

  ⚠ SPI1 挂在 APB2（72MHz），所以 256分频=281kHz、64分频=1.125MHz。
    哪天 SPI1 的时钟源变了，这两个数全得重算
  ⚠ 模块 VCC 必须接 5V（板载 LDO + 电平转换）。接 3.3V 会让 LDO 掉出稳压区、
    卡只拿到 2.5V 左右，低于规范的 2.7V 下限——症状是"能认卡、一写就失败"
  ⚠ 上电到 SD_Init() 之间 PA4 是浮空输入（约100ms），良性：
    CMD0 会把卡强行拉进 SPI 模式，不受这期间的电平影响*/

#endif
