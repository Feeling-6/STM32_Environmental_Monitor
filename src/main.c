#include "stm32f10x.h"
#include "OLED.h"
#include "SHT30.h"
#include "AD.h"
#include "Key.h"
#include "LED.h"
#include "Buzzer.h"
#include "Delay.h"

void SystemClock_Config_72MHz(void);

/*调试界面用的累计计数：单击/双击/长按/长按重复*/
static uint16_t s_Cnt[4] = {0, 0, 0, 0};

/*主循环帧计数，每轮+1。用来盯着"蜂鸣器有没有阻塞主循环"：
  它要是卡顿，说明哪次Beep里有阻塞延时。见Buzzer.h的"非阻塞"约定*/
static uint16_t s_Frame = 0;

/*按键号→音高，下标0=按键1。上行音阶，按1到6音调依次升高，
  一耳朵就能听出按的是哪个键*/
static const uint16_t s_NoteFreq[KEY_COUNT] = {
	BUZZER_NOTE_C5, BUZZER_NOTE_D5, BUZZER_NOTE_E5,
	BUZZER_NOTE_G5, BUZZER_NOTE_A5, BUZZER_NOTE_C6,
};

/*事件类型→时长(ms)，下标就是KeyEventType，[0]是"无事件"占位。
  单击短促、双击和长按长一点、长按重复要短——重复音每200ms来一次，
  单次太长会连成一片听不出节奏*/
static const uint16_t s_EventMs[5] = {0, 100, 200, 200, 60};

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
	Buzzer_Init();
	Key_Init();								//放最后：SWJ重映射会清AFIO->MAPR，别抹掉别人的

	/*静态标签只画一次*/
	OLED_ShowString(1, 1, "C   D   L   R");	//计数标题
	OLED_ShowString(2, 1, "123456");		//按键编号
	OLED_ShowString(3, 1, "......");		//实时按下状态
	OLED_ShowString(4, 1, "K- ------");		//最近事件

	/*上电自检：听到"嘀"一声，就说明PB1接线 + TIM3时钟 + PWM通路全是通的，
	  不用先按按键。非阻塞，不会拖慢首屏绘制*/
	Buzzer_Beep(BUZZER_NOTE_C6, 100);

	while (1)
	{
		/*把事件队列取干净*/
		while (Key_GetEvent(&ev))
		{
			LED_Toggle();					//每个事件翻转一次LED：没OLED也能验证驱动
			s_Cnt[ev.Event - 1] ++;

			/*响一声：按键号决定音高，事件类型决定时长。非阻塞，立刻返回*/
			Buzzer_Beep(s_NoteFreq[ev.Key - 1], s_EventMs[ev.Event]);

			OLED_ShowChar(4, 2, (char)(ev.Key + '0'));
			OLED_ShowString(4, 4, Key_EventName(ev.Event));
			OLED_ShowNum(4, 10, s_NoteFreq[ev.Key - 1], 4);	//本次音高，和听到的声音对照
			OLED_ShowString(4, 14, "Hz");

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

		/*非阻塞观测窗。左边是帧计数：蜂鸣器只要在哪次Beep里阻塞了主循环，
		  这个数就会肉眼可见地卡顿。右边是BEEP标志，闪一下就说明确实在响。
		  这两样是"非阻塞"唯一的现场证据*/
		OLED_ShowNum(3, 8, s_Frame ++, 5);
		OLED_ShowString(3, 13, Buzzer_IsBusy() ? "BEEP" : "    ");

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
