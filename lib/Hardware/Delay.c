#include "stm32f10x.h"                  // Device header

/*SysTick以HCLK为基准。本工程SystemClock_Config_72MHz()固定配到72MHz，
  而本工程的SystemInit()并不会设置SystemCoreClock（它停留在初始化的8000000），
  所以这里不能用SystemCoreClock/1000000，必须用固定值72。
  若要改成动态获取，需先在SystemClock_Config_72MHz()末尾调用SystemCoreClockUpdate()。*/
#define DELAY_CLK_MHZ		72

/**
  * @brief  微秒级延时（阻塞式轮询，不产生中断）
  * @param  xus 延时长度，范围：1~100000
  * @retval 无
  * @note   单次最长约233ms（24位LOAD上限），超过会被截断为更短的延时
  */
void Delay_us(uint32_t xus)
{
	uint32_t Reload = DELAY_CLK_MHZ * xus;

	if (Reload > 0xFFFFFF)				//SysTick->LOAD是24位，防止溢出
	{
		Reload = 0xFFFFFF;
	}

	SysTick->LOAD = Reload;
	SysTick->VAL  = 0x00;
	SysTick->CTRL = 0x00000005;			//HCLK不分频，不使能中断
	while ((SysTick->CTRL & 0x00010000) == 0);	//等待COUNTFLAG置位
	SysTick->CTRL = 0x00000000;			//关闭SysTick
}

/**
  * @brief  毫秒级延时
  * @param  xms 延时长度
  * @retval 无
  */
void Delay_ms(uint32_t xms)
{
	while (xms--)
	{
		Delay_us(1000);
	}
}

/**
  * @brief  秒级延时
  * @param  xs 延时长度
  * @retval 无
  */
void Delay_s(uint32_t xs)
{
	while (xs--)
	{
		Delay_ms(1000);
	}
}
