#include "stm32f10x.h"                  // Device header
#include "SHT30.h"
#include "I2C2.h"
#include "Delay.h"

/*SHT30的7位地址0x44(ADDR脚接地)左移1位。
  若模块的ADDR脚接了VDD，地址是7位0x45，这里要改成0x8A。*/
#define SHT30_I2C_ADDR			0x88

#define SHT30_CMD_MEASURE_HIGH	0x2400	//单次测量，高重复度，禁止时钟拉伸
#define SHT30_CMD_SOFT_RESET	0x30A2	//软复位

/**
  * @brief  发送一条16位命令（MSB在前）
  * @param  Cmd 命令字
  * @retval 0 成功，1 失败
  */
static uint8_t SHT30_WriteCmd(uint16_t Cmd)
{
	uint8_t Buf[2];

	Buf[0] = (uint8_t)(Cmd >> 8);
	Buf[1] = (uint8_t)(Cmd & 0x00FF);

	return I2C2_Write(SHT30_I2C_ADDR, Buf, 2);
}

/**
  * @brief  SHT30专用的CRC-8校验
  * @param  Data 要校验的数据
  * @param  Len 字节数
  * @retval 计算出的校验值
  * @note   多项式0x31（x^8+x^5+x^4+1），初值0xFF，逐字节MSB优先。
  *         已用数据手册样例核对：字节 0xBE 0xEF 的结果应为 0x92。
  */
static uint8_t SHT30_CRC8(const uint8_t *Data, uint8_t Len)
{
	uint8_t crc = 0xFF;
	uint8_t i, j;

	for (i = 0; i < Len; i ++)
	{
		crc ^= Data[i];

		for (j = 0; j < 8; j ++)
		{
			if (crc & 0x80)
			{
				crc = (uint8_t)((crc << 1) ^ 0x31);
			}
			else
			{
				crc = (uint8_t)(crc << 1);
			}
		}
	}

	return crc;
}

/**
  * @brief  SHT30初始化
  * @param  无
  * @retval SHT30_OK 成功，SHT30_ERR_I2C 器件无应答
  * @note   返回值即存在性检查：软复位命令发不出去（器件不ACK）就说明没接好，
  *         比"屏幕没反应不知道是哪出错"清晰得多。
  */
uint8_t SHT30_Init(void)
{
	I2C2_BusInit();						//可重复调用，OLED已初始化过则直接返回

	if (SHT30_WriteCmd(SHT30_CMD_SOFT_RESET))
	{
		return SHT30_ERR_I2C;
	}

	Delay_ms(2);						//软复位后需等待>=1.5ms

	return SHT30_OK;
}

/**
  * @brief  读取一次温湿度（阻塞式，耗时约16ms）
  * @param  Temperature 输出温度，单位摄氏度
  * @param  Humidity 输出相对湿度，单位百分比
  * @retval SHT30_OK 成功，SHT30_ERR_I2C / SHT30_ERR_CRC 失败
  * @note   用0x2400(禁止时钟拉伸)：发完命令立即返回，本函数自己Delay_ms(16)
  *         等测量完成再读。若改用带时钟拉伸的0x2C06，传感器会拉低SCL约15ms，
  *         而I2C2_TIMEOUT只有约3.5ms，必然超时失败。
  *         返回6字节：T_MSB, T_LSB, T_CRC, RH_MSB, RH_LSB, RH_CRC，
  *         每2字节数据跟1字节CRC。
  */
uint8_t SHT30_ReadData(float *Temperature, float *Humidity)
{
	uint8_t Buf[6];
	uint16_t RawT, RawH;

	if (SHT30_WriteCmd(SHT30_CMD_MEASURE_HIGH))
	{
		return SHT30_ERR_I2C;
	}

	Delay_ms(16);						//高重复度单次测量最长15ms

	if (I2C2_Read(SHT30_I2C_ADDR, Buf, 6))
	{
		return SHT30_ERR_I2C;
	}

	if (SHT30_CRC8(&Buf[0], 2) != Buf[2])		//温度两字节的校验
	{
		return SHT30_ERR_CRC;
	}

	if (SHT30_CRC8(&Buf[3], 2) != Buf[5])		//湿度两字节的校验
	{
		return SHT30_ERR_CRC;
	}

	RawT = (uint16_t)((Buf[0] << 8) | Buf[1]);
	RawH = (uint16_t)((Buf[3] << 8) | Buf[4]);

	/*换算全程保持在单精度，字面量带f后缀避免隐式提升成double走软双精度*/
	*Temperature = -45.0f + 175.0f * ((float)RawT / 65535.0f);
	*Humidity    = 100.0f * ((float)RawH / 65535.0f);

	return SHT30_OK;
}
