#ifndef __OLED_H
#define __OLED_H

#include "stm32f10x.h"                  // Device header

/*屏幕尺寸：0.96寸 128x64，8个页，每页8像素高*/
#define OLED_WIDTH		128
#define OLED_HEIGHT		64
#define OLED_PAGES		8

/*显存，1KB。绘图函数都写进这里，调用OLED_Update()才真正刷到屏幕*/
extern uint8_t OLED_DisplayBuf[OLED_PAGES][OLED_WIDTH];

/*核心*/
void OLED_Init(void);
void OLED_Update(void);
void OLED_Clear(void);

/*字符与数字：Line行位置1~4，Column列位置1~16*/
void OLED_ShowChar(uint8_t Line, uint8_t Column, char Char);
void OLED_ShowString(uint8_t Line, uint8_t Column, char *String);
void OLED_ShowNum(uint8_t Line, uint8_t Column, uint32_t Number, uint8_t Length);
void OLED_ShowSignedNum(uint8_t Line, uint8_t Column, int32_t Number, uint8_t Length);
void OLED_ShowHexNum(uint8_t Line, uint8_t Column, uint32_t Number, uint8_t Length);
void OLED_ShowBinNum(uint8_t Line, uint8_t Column, uint32_t Number, uint8_t Length);
void OLED_ShowFloatNum(uint8_t Line, uint8_t Column, double Number, uint8_t IntLength, uint8_t FraLength);

/*图形：X范围0~127，Y范围0~63*/
void OLED_DrawPoint(uint8_t X, uint8_t Y);
void OLED_ClearPoint(uint8_t X, uint8_t Y);
void OLED_DrawLine(uint8_t X0, uint8_t Y0, uint8_t X1, uint8_t Y1);
void OLED_DrawRectangle(uint8_t X, uint8_t Y, uint8_t Width, uint8_t Height, uint8_t IsFilled);
void OLED_DrawProgressBar(uint8_t X, uint8_t Y, uint8_t Width, uint8_t Height, uint8_t Progress);

/*诊断：0=正常，非0=曾经发生过I2C超时（配合调试器Watch窗口观察）*/
uint8_t OLED_GetError(void);

#endif
