#ifndef __SHT30_H
#define __SHT30_H

#include "stm32f10x.h"                  // Device header

/*状态码*/
#define SHT30_OK		0		//成功
#define SHT30_ERR_I2C	1		//I2C传输失败，或器件无应答（多半是没接好/地址不对）
#define SHT30_ERR_CRC	2		//CRC校验失败，数据被干扰

uint8_t SHT30_Init(void);
uint8_t SHT30_ReadData(float *Temperature, float *Humidity);

#endif
