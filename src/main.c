#include "stm32f10x.h"
#include "OLED.h"
#include "SHT30.h"
#include "AD.h"
#include "Key.h"
#include "LED.h"
#include "Buzzer.h"
#include "SD.h"
#include "Delay.h"

void SystemClock_Config_72MHz(void);

/*按键6兼作SD卡的操作键：单击/双击=重新初始化，长按=跑写测试。
  其余按键照常响蜂鸣器（音高不变）*/
#define KEY_ID_SD			6

/*是否需要在下一帧重画SD面板。★只在状态变化时置位——
  绝不能每帧重读卡，读卡是阻塞的（一次块读几十毫秒）*/
static uint8_t s_SdDirty = 1;

/*心跳字符的下标。它替代了原来的帧计数，作用不变：
  是"屏幕冻住"和"屏幕在转但内容旧"的唯一区分手段*/
static uint8_t s_Spin = 0;

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

/*SD卡诊断面板。四行分别是：卡型+容量 / 块0头4字节 / 引导扇区签名 / 写测试结果。
  只在需要时调用（s_SdDirty），不要每帧调*/
static void DrawSdPanel(void)
{
	uint8_t i;

	/*整屏重画而不是逐字段改：OLED_Clear只是清1KB显存，比"小心地擦掉上次
	  多写出来的字符"可靠得多*/
	OLED_Clear();

	/*---- 行1：卡型 + 容量 ----*/
	OLED_ShowString(1, 1, "SD:");
	switch (SD_Info.Type)
	{
		case SD_TYPE_SDHC: OLED_ShowString(1, 4, "SDHC"); break;
		case SD_TYPE_SDSC: OLED_ShowString(1, 4, "SDSC"); break;
		case SD_TYPE_SDXC: OLED_ShowString(1, 4, "SDXC"); break;
		default:           OLED_ShowString(1, 4, "----"); break;
	}
	if (SD_Info.CapacityMB >= 65536u)
	{
		/*★≥64GB 改用GB显示。OLED_ShowNum是按位取数的、没有溢出提示，
		  5位十进制表不到10万以上：128GB(131072MB)会显示成 31072MB——
		  和一张真31GB卡长得一模一样。换GB之后5位能表到99999GB*/
		OLED_ShowNum(1, 8, SD_Info.CapacityMB / 1024u, 5);
		OLED_ShowString(1, 13, "GB");
	}
	else if (SD_Info.CapacityMB)
	{
		/*5位而不是4位：4位会把16GB卡(15360MB)显示成 5360MB*/
		OLED_ShowNum(1, 8, SD_Info.CapacityMB, 5);
		OLED_ShowString(1, 13, "MB");
	}
	else
	{
		OLED_ShowString(1, 8, " UNKNOWN");
	}

	/*---- 行2：块0的头4字节 ----
	  没读成时这里全是FF，和"读到了FF"不是一回事——配合行4的错误码区分*/
	OLED_ShowString(2, 1, "B0:");
	for (i = 0; i < 4; i ++)
	{
		OLED_ShowHexNum(2, (uint8_t)(4 + i * 3), SD_Info.Blk0Head[i], 2);
	}

	/*---- 行3：引导扇区签名 ----
	  ★这是整个验收里最强的一条判据：FAT格式化的卡上，偏移510/511必然是
	  55 AA。只有完整、无错位、无丢字节的512字节读取才会让它落在第510字节。
	  读时序差一点、丢一个字节、RXNE没清，签名立刻错位。
	  这里显示【实际读到的两个字节】：FF FF（根本没读成）和 00 00
	  （读到了但不是引导扇区）都是NG，但病根完全不同*/
	OLED_ShowString(3, 1, "SIG:");
	OLED_ShowHexNum(3, 5, SD_Info.Blk0Tail[0], 2);
	OLED_ShowHexNum(3, 7, SD_Info.Blk0Tail[1], 2);
	if (SD_Info.Blk0Tail[0] == 0x55 && SD_Info.Blk0Tail[1] == 0xAA)
	{
		OLED_ShowString(3, 9, " OK");
	}
	else
	{
		OLED_ShowString(3, 9, " NG");
	}

	/*---- 行4：写测试结果 + 最近一次错误码 ----
	  错误码原样显示，对照SD.h里的码表就能在没有调试器的情况下定位故障*/
	OLED_ShowString(4, 1, "RW:");
	switch (SD_Info.TestResult)
	{
		case SD_TEST_RUN:  OLED_ShowString(4, 4, "RUN "); break;
		case SD_TEST_OK:   OLED_ShowString(4, 4, "OK  "); break;
		case SD_TEST_FAIL: OLED_ShowString(4, 4, "FAIL"); break;
		default:           OLED_ShowString(4, 4, "----"); break;
	}
	OLED_ShowNum(4, 9, SD_Info.LastError, 2);
}

int main(void)
{
	KeyEvent ev;

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
	SD_Init();								//放在OLED之后：SD出故障时屏幕已经可用，故障看得见
	Key_Init();								//放最后：SWJ重映射会清AFIO->MAPR，别抹掉别人的

	DrawSdPanel();
	s_SdDirty = 0;

	/*上电自检：听到"嘀"一声，就说明PB1接线 + TIM3时钟 + PWM通路全是通的。
	  非阻塞，不会拖慢首屏绘制*/
	Buzzer_Beep(BUZZER_NOTE_C6, 100);

	while (1)
	{
		/*把事件队列取干净*/
		while (Key_GetEvent(&ev))
		{
			LED_Toggle();					//每个事件翻转一次LED：没OLED也能验证驱动

			/*★按键6的长按重复【故意】不发声：写测试期间它每200ms往队列里排一个，
			  测试结束时会把结果音当场掐掉——Buzzer_Beep是替换不是叠加，
			  成功音(A5/300ms)和失败音(C5/800ms)本来靠时长区分，
			  被一个60ms的C6盖掉之后两种结果就再也分不出来了*/
			if (!(ev.Key == KEY_ID_SD && ev.Event == KEY_EVENT_LONG_REPEAT))
			{
				Buzzer_Beep(s_NoteFreq[ev.Key - 1], s_EventMs[ev.Event]);
			}

			if (ev.Key != KEY_ID_SD) continue;

			if (ev.Event == KEY_EVENT_LONG)
			{
				/*写测试要阻塞几百毫秒，先把"正在跑"刷上屏——
				  不刷的话屏幕看起来像卡死了*/
				OLED_ShowString(4, 1, "RW:RUN ");
				OLED_Update();

				SD_WriteTest();
				s_SdDirty = 1;

				/*结果音刻意避开按键6自己的C6，免得和按键音糊成一声*/
				Buzzer_Beep((SD_Info.TestResult == SD_TEST_OK) ? BUZZER_NOTE_A5
				                                               : BUZZER_NOTE_C5,
				            (SD_Info.TestResult == SD_TEST_OK) ? 300 : 800);
			}
			else if (ev.Event == KEY_EVENT_CLICK || ev.Event == KEY_EVENT_DOUBLE)
			{
				/*重新识别。双击也认：单击要等400ms双击窗口过期才上报，
				  只认单击的话"快速按两下"看起来完全没反应*/
				SD_Init();
				s_SdDirty = 1;

				Buzzer_Beep(SD_Info.Ready ? BUZZER_NOTE_A5 : BUZZER_NOTE_C5, 200);
			}
			/*★KEY_EVENT_LONG_REPEAT 【故意】不处理，不是漏了：
			  长按按键6时每200ms还会再报一次、最多50次，不挡掉的话写测试会
			  连跑几十遍——界面卡十几秒，卡也被白写几十次。*/
		}

		if (s_SdDirty)
		{
			DrawSdPanel();
			s_SdDirty = 0;
		}

		/*心跳。它转了就说明主循环还活着。
		  ★但停下来【不一定】是故障：按按键6 会故意阻塞（重新识别约100ms~1.1s，
		  写测试几百ms），那期间它本来就不动。区分"阻塞"和"卡死"要看它是否恢复：
		  恢复了 = 只是阻塞；一直不动 = 某个等待循环的超时没生效*/
		OLED_ShowChar(4, 16, "|/-\\"[s_Spin & 3]);
		s_Spin ++;

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
