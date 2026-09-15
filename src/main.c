#include "stm32f10x.h"
#include "OLED.h"
#include "SHT30.h"
#include "AD.h"
#include "Key.h"
#include "LED.h"
#include "Delay.h"

void SystemClock_Config_72MHz(void);

/*调试界面用的累计计数：单击/双击/长按/长按重复*/
static uint16_t s_Cnt[4] = {0, 0, 0, 0};

int main(void)
{
	KeyEvent ev;
	uint8_t i;
	uint8_t latch = 0;						//本帧内按下过的按键位图
	static uint8_t lastLit[KEY_COUNT] = {0};

	SystemClock_Config_72MHz();				//必须最先

	/*全局优先级分组，必须在任何NVIC_Init之前。
	  放这里而不是Key_Init()里：NVIC_Init是按当前AIRCR.PRIGROUP算优先级位偏移的，
	  而NVIC->IP[]写进去就不再重算。以后别的模块用不同分组再调一次，
	  已写入的数值不会更新但含义变了，会变成很难发现的优先级错乱。*/
	NVIC_PriorityGroupConfig(NVIC_PriorityGroup_2);

	OLED_Init();
	SHT30_Init();							//调试阶段先不显示，但保持初始化以验证驱动没坏
	AD_Init();								//同上

	LED_Init();
	Key_Init();								//放最后：SWJ重映射会清AFIO->MAPR，别抹掉别人的

	/*静态标签只画一次*/
	OLED_ShowString(1, 1, "C   D   L   R");	//计数标题
	OLED_ShowString(2, 1, "123456");		//按键编号
	OLED_ShowString(3, 1, "......");		//实时按下状态
	OLED_ShowString(4, 1, "K- ------");		//最近事件

	while (1)
	{
		/*把事件队列取干净*/
		while (Key_GetEvent(&ev))
		{
			LED_Toggle();					//每个事件翻转一次LED：没OLED也能验证驱动
			s_Cnt[ev.Event - 1] ++;

			OLED_ShowChar(4, 2, (char)(ev.Key + '0'));
			OLED_ShowString(4, 4, Key_EventName(ev.Event));

			OLED_ShowNum(1, 2,  s_Cnt[0], 2);
			OLED_ShowNum(1, 6,  s_Cnt[1], 2);
			OLED_ShowNum(1, 10, s_Cnt[2], 2);
			OLED_ShowNum(1, 14, s_Cnt[3], 2);
		}

		/*实时按下状态。用"本帧内按下过"的锁存而不是直接采样：
		  一帧约80ms(50ms延时+30ms整屏刷新)，直接采样的话短于80ms的轻碰
		  会完全看不见，会误以为那个键坏了。锁存保证任何一次按下至少点亮一帧。*/
		for (i = 0; i < KEY_COUNT; i ++)
		{
			if (Key_IsDown(i + 1))
			{
				latch |= (uint8_t)(1 << i);
			}
		}

		for (i = 0; i < KEY_COUNT; i ++)
		{
			uint8_t lit = (uint8_t)((latch >> i) & 1);

			if (lit != lastLit[i])			//只在变化时重画
			{
				lastLit[i] = lit;
				OLED_ShowChar(3, i + 1, lit ? '#' : '.');
			}
		}

		latch = 0;							//本帧用完就清

		OLED_Update();
		Delay_ms(50);
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
