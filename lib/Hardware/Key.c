#include "stm32f10x.h"                  // Device header
#include "Key.h"

/*==================== 配置 ====================*/

/*TIM2挂在APB1上。APB1分频系数是2(36MHz)，定时器时钟翻倍 = 72MHz。
  PSC=7200-1 → 10kHz；ARR=100-1 → 100Hz = 10ms扫描周期。*/
#define KEY_TIM				TIM2
#define KEY_TIM_PSC			(7200 - 1)
#define KEY_TIM_ARR			(100 - 1)

#define KEY_QUEUE_SIZE		16		//必须是2的幂也无所谓，用%取模，改大改小都行

/*==================== 按键引脚 ====================*/

typedef struct
{
	GPIO_TypeDef *Port;
	uint16_t Pin;
} KeyPinDef;

/*顺序即按键编号1~6，与引脚分配表一致。全部上拉输入，按下=低电平*/
static const KeyPinDef s_KeyPin[KEY_COUNT] =
{
	{GPIOB, GPIO_Pin_13},		//按键1
	{GPIOB, GPIO_Pin_15},		//按键2
	{GPIOA, GPIO_Pin_12},		//按键3
	{GPIOB, GPIO_Pin_3},		//按键4 —— PB3/JTDO，靠下面的JTAG重映射释放
	{GPIOB, GPIO_Pin_5},		//按键5
	{GPIOB, GPIO_Pin_7},		//按键6
};

/*==================== 每个按键的状态 ====================*/

/*状态机状态*/
#define KEY_STATE_IDLE		0		//空闲
#define KEY_STATE_PRESSED	1		//第一击按下中，正在计长按时长
#define KEY_STATE_WAIT		2		//第一击已松，等第二击（双击窗口）
#define KEY_STATE_PRESSED2	3		//第二击按下中
#define KEY_STATE_LONG		4		//长按已报，等松开

typedef struct
{
	/*第一层：消抖预滤波*/
	uint8_t  Raw;			//上次原始采样，1=按下
	uint8_t  Stable;		//消抖后的电平，1=按下
	uint8_t  DebCnt;		//消抖计数

	/*上电/卡键保护：0=未武装，必须先见到一次"松开"才让状态机工作*/
	uint8_t  Armed;

	/*第二层：状态机*/
	uint8_t  State;			//KEY_STATE_xxx
	uint16_t Cnt;			//当前状态的驻留计数
	uint16_t RepeatCnt;		//长按重复已报次数
} KeyStateDef;

/*volatile：中断里写、主循环里读（Key_IsDown）*/
static volatile KeyStateDef s_Key[KEY_COUNT];

/*==================== 事件环形队列（无锁，单生产者单消费者）====================*/

static volatile KeyEvent s_Queue[KEY_QUEUE_SIZE];
static volatile uint8_t  s_Head = 0;		//只由中断写（生产者）
static volatile uint8_t  s_Tail = 0;		//只由主循环写（消费者）
static volatile uint16_t s_Dropped = 0;

/**
  * @brief  把事件推进队列（在中断里调用）
  * @note   s_Queue 必须保持 volatile：它保证"先写载荷、再发布s_Head"这个顺序
  *         不被编译器重排。去掉volatile的话编译器可以把载荷写挪到指针发布之后，
  *         消费者就会读到残缺槽位。
  *         队列满时丢弃【最新】事件而不是最旧：想丢最旧就得让生产者推进s_Tail，
  *         而那是消费者的变量，会引入需要临界区或CAS的竞争，无锁性就没了。
  */
static void Key_PushEvent(uint8_t Key, uint8_t Event)
{
	uint8_t next = (uint8_t)((s_Head + 1) % KEY_QUEUE_SIZE);

	if (next != s_Tail)
	{
		s_Queue[s_Head].Key   = Key;
		s_Queue[s_Head].Event = Event;
		s_Head = next;					//最后才发布头指针
	}
	else
	{
		s_Dropped ++;
	}
}

/*==================== 扫描 + 状态机（在中断里跑）====================*/

/**
  * @brief  对单个按键跑一次：消抖 + 状态机
  * @param  Index 按键下标 0~KEY_COUNT-1
  * @note   状态转移图见下方注释。本函数在中断上下文里跑，绝对不能调Delay
  *         （Delay.c结尾会把SysTick关掉，会让主循环的延时永久挂死）。
  */
static void Key_ScanKey(uint8_t Index)
{
	uint8_t raw;

	/*读引脚：上拉输入，按下=低电平，取反成 1=按下*/
	raw = (GPIO_ReadInputDataBit(s_KeyPin[Index].Port, s_KeyPin[Index].Pin) == 0) ? 1 : 0;

	/*-------- 第一层：消抖预滤波 --------
	  需要连续 KEY_DEBOUNCE_TICKS 次采样一致才改变Stable。
	  这样下面的状态机只需要看消抖后的布尔值，一个消抖状态都不用写。*/
	if (raw == s_Key[Index].Raw)
	{
		if (s_Key[Index].DebCnt < KEY_DEBOUNCE_TICKS)
		{
			s_Key[Index].DebCnt ++;
		}
	}
	else
	{
		s_Key[Index].Raw = raw;
		s_Key[Index].DebCnt = 0;		//采样变了，计数重来
	}

	if (s_Key[Index].DebCnt >= KEY_DEBOUNCE_TICKS)
	{
		s_Key[Index].Stable = s_Key[Index].Raw;
	}

	/*-------- 上电/卡键保护 --------
	  必须先见到一次"松开"才允许状态机工作。否则上电时手指正按着键，
	  会在一秒后莫名报一次LONG然后开始刷LONG_REPEAT。*/
	if (s_Key[Index].Armed == 0)
	{
		if (s_Key[Index].Stable == 0)
		{
			s_Key[Index].Armed = 1;
		}
		return;
	}

	/*-------- 第二层：状态机 --------
	                    ①IDLE 空闲
	                       │ Stable==1
	                       ↓
	                  ②PRESSED 第一击按下中
	          Stable==0 ／            ＼ ++Cnt>=LONG_TICKS
	                    ↓              ↓ 发LONG
	        ③WAIT 等第二击         ④LONG 长按已报，等松开
	     Stable==1／    ＼超时        │ Stable==0 → 回①
	              ↓       ↓ 发CLICK   │（按住则每REPEAT_TICKS发LONG_REPEAT）
	   ⑤PRESSED2       回①          回①
	       │ Stable==0 → 回①
	       └ 按住>=LONG_TICKS → 放弃本次按下，回①并要求先松开
	*/
	switch (s_Key[Index].State)
	{
		case KEY_STATE_IDLE:
			if (s_Key[Index].Stable)
			{
				s_Key[Index].State     = KEY_STATE_PRESSED;
				s_Key[Index].Cnt       = 0;
				s_Key[Index].RepeatCnt = 0;
			}
			break;

		case KEY_STATE_PRESSED:
			if (s_Key[Index].Stable == 0)
			{
				/*松开了，进双击窗口等待。这里不发事件——
				  要等窗口超时才能确定它不是双击的第一击*/
				s_Key[Index].State = KEY_STATE_WAIT;
				s_Key[Index].Cnt   = 0;
			}
			else if (++ s_Key[Index].Cnt >= KEY_LONG_TICKS)
			{
				Key_PushEvent(Index + 1, KEY_EVENT_LONG);
				s_Key[Index].State = KEY_STATE_LONG;
				s_Key[Index].Cnt   = 0;
			}
			break;

		case KEY_STATE_LONG:
			/*本状态唯一的存在理由：吞掉松开动作。
			  如果长按报完直接回IDLE，而按键还按着(Stable仍是1)，
			  IDLE会立刻把它当成一次新的按下 → 再按住1秒又报一次LONG，无限刷。*/
			if (s_Key[Index].Stable == 0)
			{
				s_Key[Index].State = KEY_STATE_IDLE;
				s_Key[Index].Cnt   = 0;
			}
			else if (++ s_Key[Index].Cnt >= KEY_REPEAT_TICKS)
			{
				if (s_Key[Index].RepeatCnt < KEY_REPEAT_MAX)
				{
					Key_PushEvent(Index + 1, KEY_EVENT_LONG_REPEAT);
					s_Key[Index].RepeatCnt ++;
				}
				s_Key[Index].Cnt = 0;
			}
			break;

		case KEY_STATE_WAIT:
			if (s_Key[Index].Stable)
			{
				/*第二击按下就报DOUBLE，不等松开——菜单要响应快。
				  之所以敢这么做，是因为CLICK和DOUBLE在转移表里天然互斥
				  （WAIT只能从二者之一退出），不会出现"先报单击再报双击"的鬼影。*/
				Key_PushEvent(Index + 1, KEY_EVENT_DOUBLE);
				s_Key[Index].State = KEY_STATE_PRESSED2;
				s_Key[Index].Cnt   = 0;
			}
			else if (++ s_Key[Index].Cnt >= KEY_DOUBLE_TICKS)
			{
				Key_PushEvent(Index + 1, KEY_EVENT_CLICK);
				s_Key[Index].State = KEY_STATE_IDLE;
				s_Key[Index].Cnt   = 0;
			}
			break;

		case KEY_STATE_PRESSED2:
			if (s_Key[Index].Stable == 0)
			{
				s_Key[Index].State = KEY_STATE_IDLE;
				s_Key[Index].Cnt   = 0;
			}
			else if (++ s_Key[Index].Cnt >= KEY_LONG_TICKS)
			{
				/*超时逃逸：第二击按住超过1秒，放弃本次按下。
				  没有这条的话本状态会永远停住——不产生任何事件、状态永不改变，
				  那个键在物理松开之前完全失效（按键卡住时就是永久失效）。
				  规则：每个等待"松开"的状态都必须有超时出口。*/
				s_Key[Index].State = KEY_STATE_IDLE;
				s_Key[Index].Cnt   = 0;
				s_Key[Index].Armed = 0;		//本次按下作废，要求先松开才能重新武装
			}
			break;

		default:							//理论到不了，兜底防跑飞
			s_Key[Index].State = KEY_STATE_IDLE;
			s_Key[Index].Cnt   = 0;
			break;
	}
}

/**
  * @brief  一次完整扫描：全部6个按键各走一步
  */
static void Key_ScanTick(void)
{
	uint8_t i;

	for (i = 0; i < KEY_COUNT; i ++)
	{
		Key_ScanKey(i);
	}
}

/**
  * @brief  TIM2更新中断——按键扫描的心跳
  * @note   TIM_ClearITPendingBit 必须有。F1的TIM更新标志UIF只能软件清，
  *         漏了的话 UIE=1 且 UIF=1 会让NVIC线持续有效，中断返回后立刻重新挂起，
  *         主循环永远回不去。现象是屏幕冻在最后一帧像死机，但SWD还能连。
  */
void TIM2_IRQHandler(void)
{
	if (TIM_GetITStatus(KEY_TIM, TIM_IT_Update) == SET)
	{
		Key_ScanTick();

		TIM_ClearITPendingBit(KEY_TIM, TIM_IT_Update);
	}
}

/*==================== 对外接口 ====================*/

/**
  * @brief  按键初始化
  * @param  无
  * @retval 无
  * @note   必须在SystemClock_Config_72MHz()之后调用：PSC是按72MHz算的，
  *         若在切时钟前跑(还在8MHz HSI)，扫描周期会变成90ms，
  *         表现为"按键发木、长按要按9秒"，极难联想到时钟。
  */
void Key_Init(void)
{
	GPIO_InitTypeDef GPIO_InitStructure;
	TIM_TimeBaseInitTypeDef TIM_TimeBaseStructure;
	NVIC_InitTypeDef NVIC_InitStructure;
	uint8_t i;

	/*开启时钟。AFIO必须在里面——GPIO_PinRemapConfig是直接写AFIO->MAPR寄存器的，
	  AFIO时钟没开的话这次写会被静默丢弃，PB3就还是JTDO，按键4永远读不到。*/
	RCC_APB2PeriphClockCmd(RCC_APB2Periph_GPIOA | RCC_APB2Periph_GPIOB |
	                       RCC_APB2Periph_AFIO, ENABLE);
	RCC_APB1PeriphClockCmd(RCC_APB1Periph_TIM2, ENABLE);

	/*释放PB3(JTDO)。这一句把PA15/PB3/PB4从JTAG手里拿回来做普通GPIO，
	  同时【完整保留SWD】(PA13/PA14)，ST-Link下载调试不受影响。
	  绝对不能用 GPIO_Remap_SWJ_Disable —— 那个把SWD也关了，会连不上ST-Link。

	  必须在GPIO_Init之前做：JTAG功能优先级高于CRH/CRL配置，
	  先配PB3为输入再重映射的话PB3仍是JTDO。

	  注意副作用：这个调用会把AFIO->MAPR低24位全清掉(SPL为了对付
	  SWJ_CFG只写不可读而做的处理)，也就是抹掉所有外设重映射。
	  所以本函数要在main()里最后调用，且以后任何GPIO_PinRemapConfig
	  都要放在它之后。*/
	GPIO_PinRemapConfig(GPIO_Remap_SWJ_JTAGDisable, ENABLE);

	/*按键GPIO：上拉输入，按下=低。
	  逐个引脚调用，不用合并掩码——GPIO_Init会重写掩码里每一位的CNF/MODE，
	  一个手滑写成GPIO_Pin_All就会把PB10/PB11(I2C2，OLED+SHT30全挂)、
	  PA0/PA1(ADC)、甚至PA13/PA14(SWD)一起毁掉。逐个调用天然不可能误伤。*/
	GPIO_InitStructure.GPIO_Mode  = GPIO_Mode_IPU;
	GPIO_InitStructure.GPIO_Speed = GPIO_Speed_50MHz;

	for (i = 0; i < KEY_COUNT; i ++)
	{
		GPIO_InitStructure.GPIO_Pin = s_KeyPin[i].Pin;
		GPIO_Init(s_KeyPin[i].Port, &GPIO_InitStructure);
	}

	/*TIM2初始化：10ms周期*/
	TIM_InternalClockConfig(KEY_TIM);
	TIM_TimeBaseStructure.TIM_ClockDivision     = TIM_CKD_DIV1;
	TIM_TimeBaseStructure.TIM_CounterMode       = TIM_CounterMode_Up;
	TIM_TimeBaseStructure.TIM_Period            = KEY_TIM_ARR;
	TIM_TimeBaseStructure.TIM_Prescaler         = KEY_TIM_PSC;
	TIM_TimeBaseStructure.TIM_RepetitionCounter = 0;	//TIM2没有RCR，此项被忽略
	TIM_TimeBaseInit(KEY_TIM, &TIM_TimeBaseStructure);

	/*TIM_TimeBaseInit内部会写EGR产生一次更新事件把UIF置起来，
	  这里先清掉，避免一上电就误触发一次中断*/
	TIM_ClearFlag(KEY_TIM, TIM_IT_Update);
	TIM_ITConfig(KEY_TIM, TIM_IT_Update, ENABLE);

	/*NVIC。注意 NVIC_PriorityGroupConfig 不在这里调——它是全局设置，
	  放main()里。因为NVIC_Init是按当前AIRCR.PRIGROUP算优先级位偏移的，
	  而NVIC->IP[]写进去就不再重算：以后别的模块用不同分组再调一次，
	  已写入的优先级数值不会更新但含义变了，会变成很难发现的优先级错乱。*/
	NVIC_InitStructure.NVIC_IRQChannel                   = TIM2_IRQn;
	NVIC_InitStructure.NVIC_IRQChannelPreemptionPriority = 2;
	NVIC_InitStructure.NVIC_IRQChannelSubPriority        = 2;
	NVIC_InitStructure.NVIC_IRQChannelCmd                = ENABLE;
	NVIC_Init(&NVIC_InitStructure);

	/*最后才启动，前面全部配好之前不会有中断进来*/
	TIM_Cmd(KEY_TIM, ENABLE);
}

/**
  * @brief  取一个按键事件
  * @param  Event 输出，取到的事件
  * @retval 1 取到事件，0 队列空
  */
uint8_t Key_GetEvent(KeyEvent *Event)
{
	if (Event == 0)
	{
		return 0;
	}

	if (s_Tail == s_Head)					//空
	{
		return 0;
	}

	Event->Key   = s_Queue[s_Tail].Key;
	Event->Event = s_Queue[s_Tail].Event;
	s_Tail = (uint8_t)((s_Tail + 1) % KEY_QUEUE_SIZE);

	return 1;
}

/**
  * @brief  读取按键当前是否按下（消抖后的实时状态）
  * @param  Key 按键编号，1~KEY_COUNT
  * @retval 1 按下，0 松开（Key越界也返回0）
  */
uint8_t Key_IsDown(uint8_t Key)
{
	if (Key < 1 || Key > KEY_COUNT)			//越界保护：不拦的话会读数组外
	{
		return 0;
	}

	return s_Key[Key - 1].Stable;
}

/**
  * @brief  读取被丢弃的事件数
  * @retval 丢弃计数，正常应恒为0
  */
uint16_t Key_GetDropped(void)
{
	return s_Dropped;
}

/**
  * @brief  取事件名（固定6字符，避免调试界面残留旧字符）
  * @param  Event 事件类型
  * @retval 可显示的字符串
  */
char *Key_EventName(uint8_t Event)
{
	switch (Event)
	{
		case KEY_EVENT_CLICK:       return "CLICK ";
		case KEY_EVENT_DOUBLE:      return "DOUBLE";
		case KEY_EVENT_LONG:        return "LONG  ";
		case KEY_EVENT_LONG_REPEAT: return "REPEAT";
		default:                    return "------";
	}
}
