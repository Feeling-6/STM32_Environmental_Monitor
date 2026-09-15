#include "stm32f10x.h"                  // Device header
#include "I2C2.h"
#include "Delay.h"

/*协议层用到的引脚，总线层自己知道*/
#define I2C2_SCL_PIN		GPIO_Pin_10		//PB10 = I2C2_SCL
#define I2C2_SDA_PIN		GPIO_Pin_11		//PB11 = I2C2_SDA

static uint8_t I2C2_ErrorFlag = 0;		//0=正常，非0=曾经发生过I2C超时
static uint8_t I2C2_Ready = 0;			//0=未初始化，1=已初始化（供BusInit幂等用）

/*============================ 底层原语 ============================*/

/**
  * @brief  等待I2C事件发生，带超时保护
  * @param  I2C_EVENT 要等待的事件宏
  * @retval 0 事件已发生，1 超时（已置错误标志）
  * @note   超时把"整个程序卡死"变成"返回错误继续跑"，代价约3.5ms
  */
static uint8_t I2C2_WaitEvent(uint32_t I2C_EVENT)
{
	uint32_t Timeout = I2C2_TIMEOUT;

	while (I2C_CheckEvent(I2C2, I2C_EVENT) != SUCCESS)
	{
		if (--Timeout == 0)
		{
			I2C2_ErrorFlag = 1;
			return 1;
		}
	}

	return 0;
}

/**
  * @brief  等待总线空闲（BUSY=0）
  * @retval 0 总线空闲，1 超时（总线被从机拽死）
  */
static uint8_t I2C2_WaitIdle(void)
{
	uint32_t Timeout = I2C2_TIMEOUT;

	while (I2C_GetFlagStatus(I2C2, I2C_FLAG_BUSY) != RESET)
	{
		if (--Timeout == 0)
		{
			I2C2_ErrorFlag = 1;
			return 1;
		}
	}

	return 0;
}

/**
  * @brief  传输失败后的收尾：补一个STOP，返回1便于调用处直接return
  * @retval 恒为1
  */
static uint8_t I2C2_Abort(void)
{
	I2C_GenerateSTOP(I2C2, ENABLE);
	I2C2_ErrorFlag = 1;
	return 1;
}

/**
  * @brief  I2C总线死锁恢复
  * @retval 无
  * @note   必须在PB10/PB11配置为复用开漏之前调用，此时它们是通用开漏输出。
  *         典型现场故障：MCU在传输中途复位，从机停在拉低SDA等时钟的状态，
  *         BUSY永久置位，之后每次初始化都失败，只能拔电。
  *         补9个SCL脉冲让从机走完当前字节，再补一个STOP即可释放。
  */
static void I2C2_BusRecover(void)
{
	uint8_t i;

	GPIO_SetBits(GPIOB, I2C2_SCL_PIN | I2C2_SDA_PIN);	//释放两根线
	Delay_us(10);

	if (GPIO_ReadInputDataBit(GPIOB, I2C2_SDA_PIN) == 1)
	{
		return;											//SDA没被拽低，总线正常，不做多余时序
	}

	for (i = 0; i < 9; i ++)							//补9个时钟
	{
		GPIO_ResetBits(GPIOB, I2C2_SCL_PIN);
		Delay_us(5);
		GPIO_SetBits(GPIOB, I2C2_SCL_PIN);
		Delay_us(5);
	}

	GPIO_ResetBits(GPIOB, I2C2_SDA_PIN);				//补一个STOP条件：SCL高时SDA由低变高
	Delay_us(5);
	GPIO_SetBits(GPIOB, I2C2_SCL_PIN);
	Delay_us(5);
	GPIO_SetBits(GPIOB, I2C2_SDA_PIN);
	Delay_us(5);
}

/**
  * @brief  I2C2外设与对应GPIO初始化，可重复调用
  * @retval 无
  * @note   必须在SystemClock_Config_72MHz()之后调用：I2C_Init()由
  *         RCC_GetClocksFreq()读寄存器推算CCR，若在切到72MHz之前调用，
  *         算出的分频会小9倍，切时钟后SCL会飙升到约2MHz，远超从机上限。
  *         已初始化过则直接返回：OLED和SHT30都会调它，重复配置会把
  *         PB10/PB11短暂拉回通用开漏，中间那一瞬总线是浮空的。
  */
void I2C2_BusInit(void)
{
	GPIO_InitTypeDef GPIO_InitStructure;
	I2C_InitTypeDef I2C_InitStructure;

	if (I2C2_Ready)
	{
		return;											//已初始化，直接返回
	}

	/*开启时钟*/
	RCC_APB1PeriphClockCmd(RCC_APB1Periph_I2C2, ENABLE);	//开启I2C2的时钟
	RCC_APB2PeriphClockCmd(RCC_APB2Periph_GPIOB, ENABLE);	//开启GPIOB的时钟

	I2C_Cmd(I2C2, DISABLE);								//先关掉I2C，让外设放开引脚

	/*GPIO初始化：先配成通用开漏，供总线恢复使用*/
	GPIO_InitStructure.GPIO_Mode  = GPIO_Mode_Out_OD;
	GPIO_InitStructure.GPIO_Pin   = I2C2_SCL_PIN | I2C2_SDA_PIN;
	GPIO_InitStructure.GPIO_Speed = GPIO_Speed_50MHz;
	GPIO_Init(GPIOB, &GPIO_InitStructure);

	I2C2_BusRecover();

	/*再切成复用开漏——I2C引脚必须用这个模式*/
	GPIO_InitStructure.GPIO_Mode  = GPIO_Mode_AF_OD;
	GPIO_Init(GPIOB, &GPIO_InitStructure);

	/*I2C初始化*/
	I2C_InitStructure.I2C_Mode                = I2C_Mode_I2C;					//I2C模式
	I2C_InitStructure.I2C_DutyCycle           = I2C_DutyCycle_2;				//占空比2:1（快速模式用）
	I2C_InitStructure.I2C_OwnAddress1         = 0x00;							//STM32作为从机时的地址，用不到
	I2C_InitStructure.I2C_Ack                 = I2C_Ack_Enable;					//使能应答
	I2C_InitStructure.I2C_AcknowledgedAddress = I2C_AcknowledgedAddress_7bit;	//7位地址
	I2C_InitStructure.I2C_ClockSpeed          = I2C2_SPEED;
	I2C_Init(I2C2, &I2C_InitStructure);
	I2C_Cmd(I2C2, ENABLE);								//I2C_Init内部已置PE，此处为对齐参考实现

	I2C2_WaitIdle();

	I2C2_Ready = 1;
}

/**
  * @brief  强制重新初始化总线（含死锁恢复）
  * @retval 无
  * @note   总线卡死（SDA被从机拽低、BUSY永久置位）时调用。
  *         会短暂把PB10/PB11切回通用开漏，必须在没有传输在途时调用。
  */
void I2C2_BusReset(void)
{
	I2C2_Ready = 0;
	I2C2_BusInit();
}

/*============================ 传输 ============================*/

/**
  * @brief  主机发送：START + 地址 + 两段数据 + STOP
  * @param  Addr 已左移1位的8位从机地址
  * @param  P1,L1 第一段数据
  * @param  P2,L2 第二段数据（无则传0）
  * @retval 0 成功，1 失败（已中止传输并置错误标志）
  * @note   除最后一个字节外只等TXE(EV8)，下一字节可在当前字节还在移位时
  *         就预装进DR；最后一个字节必须等BTF(EV8_2)，否则随后的STOP会截断它。
  */
static uint8_t I2C2_Transfer(uint8_t Addr, const uint8_t *P1, uint16_t L1,
                             const uint8_t *P2, uint16_t L2)
{
	uint16_t i;

	if (I2C2_WaitIdle())
	{
		return I2C2_Abort();
	}

	/*起始条件*/
	I2C_GenerateSTART(I2C2, ENABLE);
	if (I2C2_WaitEvent(I2C_EVENT_MASTER_MODE_SELECT))
	{
		return I2C2_Abort();
	}

	/*从机地址*/
	I2C_Send7bitAddress(I2C2, Addr, I2C_Direction_Transmitter);
	if (I2C2_WaitEvent(I2C_EVENT_MASTER_TRANSMITTER_MODE_SELECTED))
	{
		return I2C2_Abort();
	}

	/*第一段*/
	for (i = 0; i < L1; i ++)
	{
		if (I2C2_WaitEvent(I2C_EVENT_MASTER_BYTE_TRANSMITTING))
		{
			return I2C2_Abort();
		}
		I2C_SendData(I2C2, P1[i]);
	}

	/*第二段*/
	for (i = 0; i < L2; i ++)
	{
		if (I2C2_WaitEvent(I2C_EVENT_MASTER_BYTE_TRANSMITTING))
		{
			return I2C2_Abort();
		}
		I2C_SendData(I2C2, P2[i]);
	}

	/*最后一个字节必须等BTF，确保数据真正移出后才发STOP*/
	if (I2C2_WaitEvent(I2C_EVENT_MASTER_BYTE_TRANSMITTED))
	{
		return I2C2_Abort();
	}

	I2C_GenerateSTOP(I2C2, ENABLE);

	return 0;
}

/**
  * @brief  接收一段数据
  * @param  Buf 接收缓冲区
  * @param  Len 字节数，必须>=1
  * @retval 0 成功，1 失败
  * @note   调用前必须已经完成 START + 读地址 + EV6（进入接收模式）。
  *         第1到倒数第2个字节保持ACK，最后一个字节前先DISABLE再GenerateSTOP，
  *         然后才等EV7把它读走——这是ST官方I2C_EE_Read和教程MPU6050的写法。
  *         结尾必须恢复ACK，否则下一次传输的第一个字节会被NACK掉。
  */
static uint8_t I2C2_ReadBody(uint8_t *Buf, uint16_t Len)
{
	I2C_AcknowledgeConfig(I2C2, ENABLE);

	while (Len)
	{
		if (Len == 1)									//最后一个字节：先给NACK再发STOP
		{
			I2C_AcknowledgeConfig(I2C2, DISABLE);
			I2C_GenerateSTOP(I2C2, ENABLE);
		}

		if (I2C2_WaitEvent(I2C_EVENT_MASTER_BYTE_RECEIVED))
		{
			return I2C2_Abort();
		}

		*Buf ++ = I2C_ReceiveData(I2C2);
		Len --;
	}

	I2C_AcknowledgeConfig(I2C2, ENABLE);				//恢复，供下次传输使用

	return 0;
}

/**
  * @brief  I2C写一串字节：START + 地址 + Buf + STOP
  * @param  Addr 已左移1位的8位从机地址
  * @param  Buf 数据缓冲区
  * @param  Len 字节数
  * @retval 0 成功，1 失败
  */
uint8_t I2C2_Write(uint8_t Addr, const uint8_t *Buf, uint16_t Len)
{
	if (Len == 0)
	{
		return 0;
	}

	return I2C2_Transfer(Addr, Buf, Len, 0, 0);
}

/**
  * @brief  I2C写一串带前缀的字节：START + 地址 + Prefix + Buf + STOP
  * @param  Addr 已左移1位的8位从机地址
  * @param  Prefix 前缀字节（控制字节/寄存器地址）
  * @param  Buf 数据缓冲区
  * @param  Len 字节数
  * @retval 0 成功，1 失败
  * @note   I2C器件最常见的"控制字节/寄存器地址 + 载荷"模式。OLED每次传输都要
  *         在数据前插一个控制字节(0x00命令/0x40数据)，若没有这个函数就得把
  *         128字节拷进临时缓冲只为在前面腾一个位置，既费栈又费时间。
  */
uint8_t I2C2_WriteStream(uint8_t Addr, uint8_t Prefix, const uint8_t *Buf, uint16_t Len)
{
	if (Len == 0)
	{
		return 0;
	}

	return I2C2_Transfer(Addr, &Prefix, 1, Buf, Len);
}

/**
  * @brief  I2C读一串字节：START + 读地址 + Buf(NACK+STOP) + STOP
  * @param  Addr 已左移1位的8位从机地址
  * @param  Buf 接收缓冲区
  * @param  Len 字节数
  * @retval 0 成功，1 失败
  */
uint8_t I2C2_Read(uint8_t Addr, uint8_t *Buf, uint16_t Len)
{
	if (Len == 0)
	{
		return 0;
	}

	if (I2C2_WaitIdle())
	{
		return I2C2_Abort();
	}

	I2C_GenerateSTART(I2C2, ENABLE);
	if (I2C2_WaitEvent(I2C_EVENT_MASTER_MODE_SELECT))
	{
		return I2C2_Abort();
	}

	I2C_Send7bitAddress(I2C2, Addr, I2C_Direction_Receiver);
	if (I2C2_WaitEvent(I2C_EVENT_MASTER_RECEIVER_MODE_SELECTED))
	{
		return I2C2_Abort();
	}

	return I2C2_ReadBody(Buf, Len);
}

/**
  * @brief  I2C先写后读（写寄存器地址，重复START，再读）：最经典的I2C器件访问模式
  * @param  Addr 已左移1位的8位从机地址
  * @param  W 要写出的数据（通常是寄存器地址）
  * @param  WLen 写出字节数
  * @param  R 接收缓冲区
  * @param  RLen 接收字节数
  * @retval 0 成功，1 失败
  */
uint8_t I2C2_WriteRead(uint8_t Addr, const uint8_t *W, uint16_t WLen,
                       uint8_t *R, uint16_t RLen)
{
	uint16_t i;

	if (I2C2_WaitIdle())
	{
		return I2C2_Abort();
	}

	/*写阶段*/
	I2C_GenerateSTART(I2C2, ENABLE);
	if (I2C2_WaitEvent(I2C_EVENT_MASTER_MODE_SELECT))
	{
		return I2C2_Abort();
	}

	I2C_Send7bitAddress(I2C2, Addr, I2C_Direction_Transmitter);
	if (I2C2_WaitEvent(I2C_EVENT_MASTER_TRANSMITTER_MODE_SELECTED))
	{
		return I2C2_Abort();
	}

	for (i = 0; i < WLen; i ++)
	{
		if (I2C2_WaitEvent(I2C_EVENT_MASTER_BYTE_TRANSMITTING))
		{
			return I2C2_Abort();
		}
		I2C_SendData(I2C2, W[i]);
	}

	/*写完后必须等BTF，确保数据真正移出，才能发重复START*/
	if (I2C2_WaitEvent(I2C_EVENT_MASTER_BYTE_TRANSMITTED))
	{
		return I2C2_Abort();
	}

	if (RLen == 0)										//只写不读，直接收尾
	{
		I2C_GenerateSTOP(I2C2, ENABLE);
		return 0;
	}

	/*重复START，切到接收*/
	I2C_GenerateSTART(I2C2, ENABLE);
	if (I2C2_WaitEvent(I2C_EVENT_MASTER_MODE_SELECT))
	{
		return I2C2_Abort();
	}

	I2C_Send7bitAddress(I2C2, Addr, I2C_Direction_Receiver);
	if (I2C2_WaitEvent(I2C_EVENT_MASTER_RECEIVER_MODE_SELECTED))
	{
		return I2C2_Abort();
	}

	return I2C2_ReadBody(R, RLen);
}

/*============================ 诊断 ============================*/

/**
  * @brief  读取总线错误标志
  * @retval 0=正常，非0=曾经发生过I2C超时
  */
uint8_t I2C2_GetError(void)
{
	return I2C2_ErrorFlag;
}

/**
  * @brief  清除总线错误标志
  * @retval 无
  */
void I2C2_ClearError(void)
{
	I2C2_ErrorFlag = 0;
}
