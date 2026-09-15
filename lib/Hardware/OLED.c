#include "stm32f10x.h"                  // Device header
#include "OLED.h"
#include "OLED_Font.h"                  //必须在stm32f10x.h之后：字库用到uint8_t，且字库自身没有include
#include "I2C2.h"                       //总线传输层，与SHT30共用
#include "Delay.h"

/*设备地址：SSD1315的7位地址0x3C左移1位
  （I2C_Send7bitAddress只改bit0，必须传左移后的值）*/
#define OLED_I2C_ADDR		0x78

/*控制字节：I2C传输中紧跟从机地址的那个字节*/
#define OLED_CTRL_CMD		0x00		//后续字节为命令
#define OLED_CTRL_DATA		0x40		//后续字节为显示数据

uint8_t OLED_DisplayBuf[OLED_PAGES][OLED_WIDTH];	//显存，1KB

/*============================ 传输层（由I2C2模块提供） ============================*/

/**
  * @brief  OLED写多个命令，一次传输
  * @note   控制字节0x00表示后续字节为命令
  */
static void OLED_WriteCommands(const uint8_t *Cmd, uint16_t Len)
{
	I2C2_WriteStream(OLED_I2C_ADDR, OLED_CTRL_CMD, Cmd, Len);
}

/**
  * @brief  OLED写显示数据，一次传输
  * @note   控制字节0x40表示后续字节为显示数据
  */
static void OLED_WriteData(const uint8_t *Data, uint16_t Len)
{
	I2C2_WriteStream(OLED_I2C_ADDR, OLED_CTRL_DATA, Data, Len);
}

/**
  * @brief  OLED设置光标位置
  * @param  Y 以左上角为原点，向下方向的页坐标，范围：0~7
  * @param  X 以左上角为原点，向右方向的列坐标，范围：0~127
  * @retval 无
  * @note   三条命令合并为一次I2C传输（参考实现是三次）。
  *         这里用页寻址模式的原生命令，不发0x20，见OLED_Init的说明。
  */
static void OLED_SetCursor(uint8_t Y, uint8_t X)
{
	uint8_t Cmd[3];

	Cmd[0] = 0xB0 | Y;						//设置页地址
	Cmd[1] = 0x10 | ((X & 0xF0) >> 4);		//设置列地址高4位
	Cmd[2] = 0x00 | (X & 0x0F);				//设置列地址低4位

	OLED_WriteCommands(Cmd, 3);
}

/*============================ L3 对外接口 ============================*/

/**
  * @brief  SSD1315初始化命令表
  * @note   整表一次I2C传输发出，参考实现是26次。
  *         刻意不发0x20：SSD1306/SSD1315上电默认就是页寻址模式，保持页寻址可以
  *         完全绕开"SSD1315在水平寻址模式下与0xB0混用时不跨页自动换行"的问题
  *         （社区大量"SSD1315不自动回绕"的报告都源于此），SSD1306则宽容得多。
  *         0x8D,0x14是SSD1315使能内部电荷泵的必需命令。
  */
static const uint8_t OLED_InitCmd[] =
{
	0xAE,				//关闭显示
	0xD5, 0x80,			//设置显示时钟分频比/振荡器频率
	0xA8, 0x3F,			//设置多路复用率 = 64
	0xD3, 0x00,			//设置显示偏移
	0x40,				//设置显示开始行
	0xA1,				//设置左右方向，0xA1正常 0xA0左右反置
	0xC8,				//设置上下方向，0xC8正常 0xC0上下反置
	0xDA, 0x12,			//设置COM引脚硬件配置
	0x81, 0xCF,			//设置对比度控制
	0xD9, 0xF1,			//设置预充电周期
	0xDB, 0x30,			//设置VCOMH取消选择级别
	0xA4,				//设置整个显示打开/关闭
	0xA6,				//设置正常/倒转显示
	0x8D, 0x14,			//设置充电泵：SSD1315必须使能内部电荷泵
	0xAF				//开启显示
};

/**
  * @brief  OLED初始化
  * @param  无
  * @retval 无
  */
void OLED_Init(void)
{
	uint8_t i, j;

	I2C2_ClearError();
	I2C2_BusInit();						//GPIO + I2C外设 + 总线恢复（可重复调用）

	Delay_ms(100);						//SSD1315上电稳定后需等待>=20ms才能发命令

	OLED_WriteCommands(OLED_InitCmd, sizeof(OLED_InitCmd));

	/*清空显存再刷一次，屏幕上电即全黑*/
	for (j = 0; j < OLED_PAGES; j ++)
	{
		for (i = 0; i < OLED_WIDTH; i ++)
		{
			OLED_DisplayBuf[j][i] = 0x00;
		}
	}

	OLED_Update();
}

/**
  * @brief  把显存刷到屏幕
  * @param  无
  * @retval 无
  * @note   每页一次传输（3条定位命令 + 128字节数据），共8次。写满128字节
  *         正好停在本页边界，不跨页。整屏约25ms@400kHz。
  */
void OLED_Update(void)
{
	uint8_t j;

	for (j = 0; j < OLED_PAGES; j ++)
	{
		OLED_SetCursor(j, 0);
		OLED_WriteData(OLED_DisplayBuf[j], OLED_WIDTH);
	}
}

/**
  * @brief  OLED清屏（清空显存并立即刷新到屏幕）
  * @param  无
  * @retval 无
  */
void OLED_Clear(void)
{
	static const uint8_t Zero[OLED_WIDTH] = {0};	//放flash，不占RAM
	uint8_t i, j;

	for (j = 0; j < OLED_PAGES; j ++)
	{
		for (i = 0; i < OLED_WIDTH; i ++)
		{
			OLED_DisplayBuf[j][i] = 0x00;
		}

		OLED_SetCursor(j, 0);
		OLED_WriteData(Zero, OLED_WIDTH);
	}
}

/**
  * @brief  OLED显示一个字符
  * @param  Line 行位置，范围：1~4
  * @param  Column 列位置，范围：1~16
  * @param  Char 要显示的一个字符，范围：ASCII可见字符
  * @retval 无
  * @note   字模库只覆盖0x20~0x7E共95个字符，超出范围的（例如中文字符串的
  *         UTF-8字节）一律用'?'代替，避免越界读取字模库
  */
void OLED_ShowChar(uint8_t Line, uint8_t Column, char Char)
{
	uint8_t i, Index;

	/*行/列越界直接返回。字符串过长时Column会超出1~16，若不拦会写到显存数组之外，
	  把相邻RAM变量踩坏（在页7时尤其危险）*/
	if (Line < 1 || Line > 4 || Column < 1 || Column > 16)
	{
		return;
	}

	if (Char < ' ' || Char > '~')
	{
		Char = '?';
	}
	Index = (uint8_t)(Char - ' ');

	for (i = 0; i < 8; i ++)
	{
		OLED_DisplayBuf[(Line - 1) * 2][(Column - 1) * 8 + i]     = OLED_F8x16[Index][i];		//上半页
		OLED_DisplayBuf[(Line - 1) * 2 + 1][(Column - 1) * 8 + i] = OLED_F8x16[Index][i + 8];	//下半页
	}
}

/**
  * @brief  OLED显示字符串
  * @param  Line 起始行位置，范围：1~4
  * @param  Column 起始列位置，范围：1~16
  * @param  String 要显示的字符串，范围：ASCII可见字符
  * @retval 无
  */
void OLED_ShowString(uint8_t Line, uint8_t Column, char *String)
{
	uint8_t i;

	for (i = 0; String[i] != '\0'; i ++)
	{
		OLED_ShowChar(Line, Column + i, String[i]);
	}
}

/**
  * @brief  OLED次方函数
  * @retval 返回值等于X的Y次方
  */
static uint32_t OLED_Pow(uint32_t X, uint32_t Y)
{
	uint32_t Result = 1;

	while (Y --)
	{
		Result *= X;
	}

	return Result;
}

/**
  * @brief  OLED显示数字（十进制，正数）
  * @param  Line 起始行位置，范围：1~4
  * @param  Column 起始列位置，范围：1~16
  * @param  Number 要显示的数字，范围：0~4294967295
  * @param  Length 要显示数字的长度，范围：1~10
  * @retval 无
  */
void OLED_ShowNum(uint8_t Line, uint8_t Column, uint32_t Number, uint8_t Length)
{
	uint8_t i;

	for (i = 0; i < Length; i ++)
	{
		OLED_ShowChar(Line, Column + i, Number / OLED_Pow(10, Length - i - 1) % 10 + '0');
	}
}

/**
  * @brief  OLED显示数字（十进制，带符号数）
  * @param  Line 起始行位置，范围：1~4
  * @param  Column 起始列位置，范围：1~16
  * @param  Number 要显示的数字，范围：-2147483648~2147483647
  * @param  Length 要显示数字的长度，范围：1~10
  * @retval 无
  */
void OLED_ShowSignedNum(uint8_t Line, uint8_t Column, int32_t Number, uint8_t Length)
{
	uint8_t i;
	uint32_t Number1;

	if (Number >= 0)
	{
		OLED_ShowChar(Line, Column, '+');
		Number1 = Number;
	}
	else
	{
		OLED_ShowChar(Line, Column, '-');
		Number1 = -Number;
	}

	for (i = 0; i < Length; i ++)
	{
		OLED_ShowChar(Line, Column + i + 1, Number1 / OLED_Pow(10, Length - i - 1) % 10 + '0');
	}
}

/**
  * @brief  OLED显示数字（十六进制，正数）
  * @param  Line 起始行位置，范围：1~4
  * @param  Column 起始列位置，范围：1~16
  * @param  Number 要显示的数字，范围：0~0xFFFFFFFF
  * @param  Length 要显示数字的长度，范围：1~8
  * @retval 无
  */
void OLED_ShowHexNum(uint8_t Line, uint8_t Column, uint32_t Number, uint8_t Length)
{
	uint8_t i, SingleNumber;

	for (i = 0; i < Length; i ++)
	{
		SingleNumber = Number / OLED_Pow(16, Length - i - 1) % 16;

		if (SingleNumber < 10)
		{
			OLED_ShowChar(Line, Column + i, SingleNumber + '0');
		}
		else
		{
			OLED_ShowChar(Line, Column + i, SingleNumber - 10 + 'A');
		}
	}
}

/**
  * @brief  OLED显示数字（二进制，正数）
  * @param  Line 起始行位置，范围：1~4
  * @param  Column 起始列位置，范围：1~16
  * @param  Number 要显示的数字，范围：0~1111 1111 1111 1111
  * @param  Length 要显示数字的长度，范围：1~16
  * @retval 无
  */
void OLED_ShowBinNum(uint8_t Line, uint8_t Column, uint32_t Number, uint8_t Length)
{
	uint8_t i;

	for (i = 0; i < Length; i ++)
	{
		OLED_ShowChar(Line, Column + i, Number / OLED_Pow(2, Length - i - 1) % 2 + '0');
	}
}

/**
  * @brief  OLED显示浮点数（十进制，带符号数）
  * @param  Line 起始行位置，范围：1~4
  * @param  Column 起始列位置，范围：1~16
  * @param  Number 要显示的数字
  * @param  IntLength 整数部分长度，范围：1~10
  * @param  FraLength 小数部分长度，范围：1~10
  * @retval 无
  * @note   全程整数运算，不调用printf——拉进newlib的浮点格式化会凭空增加10KB以上flash
  */
void OLED_ShowFloatNum(uint8_t Line, uint8_t Column, double Number, uint8_t IntLength, uint8_t FraLength)
{
	uint8_t i;
	uint32_t Number1, Number2;

	if (Number >= 0)
	{
		OLED_ShowChar(Line, Column, '+');
		Number1 = (uint32_t)Number;
		Number2 = (uint32_t)((Number - Number1) * OLED_Pow(10, FraLength) + 0.5);
	}
	else
	{
		OLED_ShowChar(Line, Column, '-');
		Number1 = (uint32_t)(-Number);
		Number2 = (uint32_t)((-Number - Number1) * OLED_Pow(10, FraLength) + 0.5);
	}

	/*小数部分四舍五入后可能进位，例如 25.99 保留1位小数应显示 26.0*/
	if (Number2 >= OLED_Pow(10, FraLength))
	{
		Number2 = 0;
		Number1 ++;
	}

	for (i = 0; i < IntLength; i ++)
	{
		OLED_ShowChar(Line, Column + i + 1, Number1 / OLED_Pow(10, IntLength - i - 1) % 10 + '0');
	}

	OLED_ShowChar(Line, Column + IntLength + 1, '.');

	for (i = 0; i < FraLength; i ++)
	{
		OLED_ShowChar(Line, Column + IntLength + i + 2, Number2 / OLED_Pow(10, FraLength - i - 1) % 10 + '0');
	}
}

/**
  * @brief  OLED画点（写入显存，需调用OLED_Update()才显示）
  * @param  X 横坐标，范围：0~127
  * @param  Y 纵坐标，范围：0~63
  * @retval 无
  */
void OLED_DrawPoint(uint8_t X, uint8_t Y)
{
	if (X >= OLED_WIDTH || Y >= OLED_HEIGHT)
	{
		return;							//越界直接丢弃，防止写到显存数组之外
	}

	OLED_DisplayBuf[Y / 8][X] |= 0x01 << (Y % 8);
}

/**
  * @brief  OLED清除一个点
  * @param  X 横坐标，范围：0~127
  * @param  Y 纵坐标，范围：0~63
  * @retval 无
  */
void OLED_ClearPoint(uint8_t X, uint8_t Y)
{
	if (X >= OLED_WIDTH || Y >= OLED_HEIGHT)
	{
		return;							//越界直接丢弃，防止写到显存数组之外
	}

	OLED_DisplayBuf[Y / 8][X] &= ~(0x01 << (Y % 8));
}

/**
  * @brief  OLED画直线（Bresenham算法，任意方向）
  * @param  X0,Y0 起点坐标
  * @param  X1,Y1 终点坐标
  * @retval 无
  */
void OLED_DrawLine(uint8_t X0, uint8_t Y0, uint8_t X1, uint8_t Y1)
{
	int16_t x = X0, y = Y0, xe = X1, ye = Y1;
	int16_t dx, dy, sx, sy, err, e2;

	dx = (xe > x) ? (xe - x) : (x - xe);
	dy = (ye > y) ? (ye - y) : (y - ye);
	sx = (x < xe) ? 1 : -1;
	sy = (y < ye) ? 1 : -1;
	err = dx - dy;

	while (1)
	{
		OLED_DrawPoint((uint8_t)x, (uint8_t)y);

		if (x == xe && y == ye)
		{
			break;
		}

		e2 = 2 * err;

		if (e2 > -dy)
		{
			err -= dy;
			x += sx;
		}

		if (e2 < dx)
		{
			err += dx;
			y += sy;
		}
	}
}

/**
  * @brief  OLED画矩形
  * @param  X,Y 左上角坐标
  * @param  Width 宽度
  * @param  Height 高度
  * @param  IsFilled 是否填充，0=空心 1=实心
  * @retval 无
  */
void OLED_DrawRectangle(uint8_t X, uint8_t Y, uint8_t Width, uint8_t Height, uint8_t IsFilled)
{
	uint8_t i, j;

	if (IsFilled)
	{
		for (i = 0; i < Height; i ++)
		{
			for (j = 0; j < Width; j ++)
			{
				OLED_DrawPoint(X + j, Y + i);
			}
		}
	}
	else
	{
		for (j = 0; j < Width; j ++)
		{
			OLED_DrawPoint(X + j, Y);
			OLED_DrawPoint(X + j, Y + Height - 1);
		}

		for (i = 0; i < Height; i ++)
		{
			OLED_DrawPoint(X, Y + i);
			OLED_DrawPoint(X + Width - 1, Y + i);
		}
	}
}

/**
  * @brief  OLED画进度条
  * @param  X,Y 左上角坐标
  * @param  Width 总宽度
  * @param  Height 总高度
  * @param  Progress 进度百分比，范围：0~100
  * @retval 无
  */
void OLED_DrawProgressBar(uint8_t X, uint8_t Y, uint8_t Width, uint8_t Height, uint8_t Progress)
{
	uint8_t Fill;

	if (Progress > 100)
	{
		Progress = 100;
	}

	OLED_DrawRectangle(X, Y, Width, Height, 0);		//画外框

	if (Width > 2 && Height > 2)					//有内部空间才画填充
	{
		Fill = (uint8_t)((uint16_t)(Width - 2) * Progress / 100);

		if (Fill > 0)
		{
			OLED_DrawRectangle(X + 1, Y + 1, Fill, Height - 2, 1);
		}
	}
}

/**
  * @brief  读取I2C错误标志
  * @retval 0=正常，非0=曾经发生过I2C超时
  * @note   OLED_Init()会把标志清零。因为屏幕本身没坏时无法自己显示错误，
  *         这个接口主要是配合调试器的Watch窗口使用。
  */
uint8_t OLED_GetError(void)
{
	return I2C2_GetError();				//总线是和SHT30共用的，错误标志也共用
}
