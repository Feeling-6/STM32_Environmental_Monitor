#include "stm32f10x.h"                  // Device header
#include "LED.h"

#define LED_PIN			GPIO_Pin_12		//PB12 = LED信号端

/**
  * @brief  LED初始化
  * @param  无
  * @retval 无
  * @note   高电平点亮，所以初始化置低电平（熄灭）
  */
void LED_Init(void)
{
	GPIO_InitTypeDef GPIO_InitStructure;

	/*开启时钟*/
	RCC_APB2PeriphClockCmd(RCC_APB2Periph_GPIOB, ENABLE);	//开启GPIOB的时钟

	/*GPIO初始化：推挽输出*/
	GPIO_InitStructure.GPIO_Mode  = GPIO_Mode_Out_PP;
	GPIO_InitStructure.GPIO_Pin   = LED_PIN;
	GPIO_InitStructure.GPIO_Speed = GPIO_Speed_50MHz;
	GPIO_Init(GPIOB, &GPIO_InitStructure);

	/*设置GPIO初始化后的默认电平：低=灭*/
	GPIO_ResetBits(GPIOB, LED_PIN);
}

/**
  * @brief  LED点亮
  * @param  无
  * @retval 无
  */
void LED_ON(void)
{
	GPIO_SetBits(GPIOB, LED_PIN);		//高电平点亮
}

/**
  * @brief  LED熄灭
  * @param  无
  * @retval 无
  */
void LED_OFF(void)
{
	GPIO_ResetBits(GPIOB, LED_PIN);		//低电平熄灭
}

/**
  * @brief  LED状态翻转
  * @param  无
  * @retval 无
  */
void LED_Toggle(void)
{
	if (GPIO_ReadOutputDataBit(GPIOB, LED_PIN) == 0)
	{
		GPIO_SetBits(GPIOB, LED_PIN);
	}
	else
	{
		GPIO_ResetBits(GPIOB, LED_PIN);
	}
}
