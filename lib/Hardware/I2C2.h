#ifndef __I2C2_H
#define __I2C2_H

#include "stm32f10x.h"                  // Device header

/*总线参数：OLED与SHT30共用，改这里两边同时生效*/
#define I2C2_SPEED		400000		//若总线不稳定（弱上拉/长线）可降为100000
#define I2C2_TIMEOUT	10000		//约3.5ms@72MHz，比一个字节时间(22.5us@400k)大150倍

/*从机地址一律传"7位地址左移1位"的8位形式：
  I2C_Send7bitAddress只改bit0然后整字节写DR，从不左移，
  所以传7位原值会寻址到别处、从机不应答。
  OLED(7位0x3C)传0x78，SHT30(7位0x44)传0x88。*/

/*总线管理：两个器件都必须在进入主循环前初始化完*/
void I2C2_BusInit(void);							//可重复调用，已初始化则直接返回
void I2C2_BusReset(void);							//强制重初始化（含死锁恢复）

/*传输：返回0成功，非0失败*/
uint8_t I2C2_Write(uint8_t Addr, const uint8_t *Buf, uint16_t Len);
uint8_t I2C2_WriteStream(uint8_t Addr, uint8_t Prefix, const uint8_t *Buf, uint16_t Len);
uint8_t I2C2_Read(uint8_t Addr, uint8_t *Buf, uint16_t Len);
uint8_t I2C2_WriteRead(uint8_t Addr, const uint8_t *W, uint16_t WLen, uint8_t *R, uint16_t RLen);

/*诊断：0=正常，非0=曾经发生过I2C超时（配合调试器Watch窗口观察）*/
uint8_t I2C2_GetError(void);
void I2C2_ClearError(void);

#endif
