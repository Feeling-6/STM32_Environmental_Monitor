#ifndef __AD_H
#define __AD_H

#include "stm32f10x.h"                  // Device header

/*通道索引：对应规则组扫描顺序(rank)，不是随便排的*/
#define AD_MQ2		0		//PA0 / ADC1_IN0，MQ-2烟雾
#define AD_LIGHT	1		//PA1 / ADC1_IN1，光敏电阻

/*MQ-2分压：上臂10k、下臂20k，V_PA0 = V_AO × 20/(10+20)，反推要乘1.5。
  5V满量程分压后是3.333V，略超VDDA(3.3V)但安全：
  从10k源灌入保护二极管的电流只有约3uA，而引脚注入限值是±5mA。
  唯一后果是AO超过约4.95V时读满量程4095（顶部1%量程被削）。*/
#define AD_MQ2_DIVIDER	1.5f

/*ADC参考电压*/
#define AD_VREF			3.3f

/*DMA搬运目标，硬件每11.33us刷新一次。
  必须volatile：DMA写内存是硬件行为，编译器看不见，不加的话主循环读它
  可能被优化成读寄存器里的陈旧值（本工程其他缓冲区如OLED_DisplayBuf
  都由CPU自己写，所以不需要，这里是例外）。
  只读，不要在外部写它——写了会和DMA打架。*/
extern volatile uint16_t AD_Value[2];

void     AD_Init(void);
uint16_t AD_GetMQ2(void);				//原始值，范围：0~4095
uint16_t AD_GetLight(void);				//原始值，范围：0~4095
float    AD_GetVoltage(uint16_t Raw);	//引脚电压，范围：0~3.3V
float    AD_GetMQ2Voltage(void);		//折算回MQ-2的AO引脚电压（含×1.5）

/*诊断：0=正常，非0=DMA传输错误/ADC溢出（读后自动清除）
  这套设计里如果DMA停了，读数会静静地变成陈旧值而没有任何报错，
  屏幕上数字看着完全正常；靠这个接口才能区分"真的在变"和"DMA早就死了"*/
uint8_t  AD_GetError(void);

#endif
