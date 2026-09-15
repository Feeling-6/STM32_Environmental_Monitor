#include "stm32f10x.h"
#include "OLED.h"
#include "SHT30.h"
#include "Delay.h"

void SystemClock_Config_72MHz(void);

int main(void)
{
	float Temp, Humi;

	SystemClock_Config_72MHz();		//必须在前：I2C_Init()依赖真实时钟推算分频

	OLED_Init();
	SHT30_Init();

	/*静态内容只画一次，之后每轮只更新数值*/
	OLED_ShowString(1, 1, "Env Monitor");
	OLED_ShowString(2, 1, "Temp:");
	OLED_ShowString(3, 1, "Humi:");
	OLED_ShowString(4, 1, "SHT30");

	while (1)
	{
		if (SHT30_ReadData(&Temp, &Humi) == SHT30_OK)
		{
			OLED_ShowFloatNum(2, 7, Temp, 2, 1);
			OLED_ShowFloatNum(3, 7, Humi, 2, 1);
			OLED_ShowString(4, 7, "OK  ");
		}
		else
		{
			OLED_ShowString(2, 7, "-----");		//空格字模是全零，正好覆盖旧值
			OLED_ShowString(3, 7, "-----");
			OLED_ShowString(4, 7, "FAIL");
		}

		OLED_Update();
		Delay_ms(1000);
	}
}

void SystemClock_Config_72MHz(void) {
    // 1. 复位 RCC 时钟配置
    RCC_DeInit();

    // 2. 开启 HSE (外部 8MHz 晶振)
    RCC_HSEConfig(RCC_HSE_ON);

    // 3. 等待外部晶振启动稳定
    if (RCC_WaitForHSEStartUp() == SUCCESS) {

        // 4. 配置 Flash 预取指和等待周期 (72MHz 必须设为 2，否则会死机)
        FLASH_PrefetchBufferCmd(FLASH_PrefetchBuffer_Enable);
        FLASH_SetLatency(FLASH_Latency_2);

        // 5. 配置总线分频 (AHB=72MHz, APB2=72MHz, APB1=36MHz)
        RCC_HCLKConfig(RCC_SYSCLK_Div1);
        RCC_PCLK2Config(RCC_HCLK_Div1);
        RCC_PCLK1Config(RCC_HCLK_Div2);

        // 6. 配置 PLL：选择 HSE 并放大 9 倍 (8MHz * 9 = 72MHz)
        RCC_PLLConfig(RCC_PLLSource_HSE_Div1, RCC_PLLMul_9);

        // 7. 启动 PLL 并等待就绪
        RCC_PLLCmd(ENABLE);
        while (RCC_GetFlagStatus(RCC_FLAG_PLLRDY) == RESET) {}

        // 8. 将系统主时钟 (SYSCLK) 切换为 PLL 输出
        RCC_SYSCLKConfig(RCC_SYSCLKSource_PLLCLK);
        while (RCC_GetSYSCLKSource() != 0x08) {} // 等待切换成功
    }
}
