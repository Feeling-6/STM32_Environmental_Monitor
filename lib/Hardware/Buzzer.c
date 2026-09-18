#include "stm32f10x.h"                  // Device header
#include "Buzzer.h"

/*注意这里【刻意不】include "Delay.h"：本模块一个阻塞延时都不能有，
  这个include的缺席本身就是"非阻塞"的证据，别加。*/

/*==================== 配置 ====================*/

/*TIM3挂在APB1上。APB1分频系数是2(36MHz)，定时器时钟翻倍 = 72MHz。
  PSC固定72-1 → 1MHz计数时钟；音调只改ARR：f = 1000000 / (ARR + 1)。
  固定PSC而不是跟着频率一起算，是为了在可听频段内保持几百级的占空比分辨率。*/
#define BUZZER_TIM			TIM3
#define BUZZER_TIM_PSC		(72 - 1)
#define BUZZER_TIMCLK_HZ	1000000UL
#define BUZZER_DUTY_PCT		50			//50%占空比最响

/*夹取范围。上限受"中断频率 = 音调频率"约束——本模块的倒计时就跑在更新中断里，
  音调设到10kHz就是每秒1万次中断。别往上调。*/
#define BUZZER_FREQ_MIN		100
#define BUZZER_FREQ_MAX		10000
#define BUZZER_MS_MAX		5000

/*倒计时剩余周期数。必须volatile：主循环在写，TIM3中断在读改写，
  理由同Key.c的s_Queue。0 = 没在响*/
static volatile uint32_t s_Remain = 0;

/*==================== 内部函数 ====================*/

/**
  * @brief  装载一个音：算ARR和脉宽、重配通道为PWM1、起倒计时
  * @param  FreqHz 音调频率，会被夹取到BUZZER_FREQ_MIN~BUZZER_FREQ_MAX
  * @param  Ms 时长，会被夹取到1~BUZZER_MS_MAX
  * @retval 无
  * @note   只写寄存器，不停定时器也不开中断源。调用者的时序责任不同：
  *         主循环调用后要补TIM_GenerateEvent(UG)把CNT清零（见Buzzer_Beep），
  *         中断里不会用到本函数（中断只负责递减和停机）。
  */
static void Buzzer_Load(uint16_t FreqHz, uint16_t Ms)
{
	TIM_OCInitTypeDef TIM_OCInitStructure;
	uint32_t Arr, Periods;

	/*夹取。ARR要放得下16位——频率低于约16Hz时它会溢出，
	  被TIM_SetAutoreload静默截断成错误的短周期*/
	if (FreqHz < BUZZER_FREQ_MIN)	FreqHz = BUZZER_FREQ_MIN;
	if (FreqHz > BUZZER_FREQ_MAX)	FreqHz = BUZZER_FREQ_MAX;
	if (Ms == 0)					Ms = 1;
	if (Ms > BUZZER_MS_MAX)			Ms = BUZZER_MS_MAX;

	/*ARR没有预装载（全工程没调过TIM_ARRPreloadConfig），写下去立即生效*/
	Arr = BUZZER_TIMCLK_HZ / FreqHz - 1;
	TIM_SetAutoreload(BUZZER_TIM, (uint16_t)Arr);

	/*重新配置通道4，不只是设脉宽：Buzzer_Stop()会把OC4M改成"强制无效"，
	  所以每次起音都必须把通道恢复成PWM1模式。TIM_OC4Init正好一次做三件事——
	  OC4M=PWM1、CCR4=脉宽、CC4E=使能输出。

	  ⚠ TIM_OCPolarity必须显式赋值。这个结构体是栈上的局部变量，而TIM_OC4Init
	    会读它去写CC4P位（stm32f10x_tim.c:552）。漏赋值就是垃圾值，一旦CC4P
	    被置1输出就反相 → 上电即长响、按键反而让它停，和"模块其实是低电平触发"
	    的现象一模一样，极难查。TIM_OCPolarity_High才是"不反相"（它=0x0000）。*/
	TIM_OCInitStructure.TIM_OCMode      = TIM_OCMode_PWM1;
	TIM_OCInitStructure.TIM_OutputState = TIM_OutputState_Enable;
	TIM_OCInitStructure.TIM_OCPolarity  = TIM_OCPolarity_High;
	TIM_OCInitStructure.TIM_Pulse       = (uint16_t)((Arr + 1) * BUZZER_DUTY_PCT / 100);
	TIM_OC4Init(BUZZER_TIM, &TIM_OCInitStructure);

	/*算倒计时。单位是PWM周期数，不是毫秒——工程里没有现成的毫秒时基
	  （Delay是阻塞轮询且独占SysTick，中断里绝对不能用）。
	  +500是四舍五入。必须显式转uint32_t：两个uint16_t相乘按int32算，
	  65535×65535会溢出成未定义行为*/
	Periods = ((uint32_t)FreqHz * Ms + 500) / 1000;
	if (Periods == 0)				Periods = 1;	//低频+极短时长会算成0，那样就永远不响

	s_Remain = Periods;
}

/*==================== 中断服务函数 ====================*/

/**
  * @brief  TIM3更新中断——蜂鸣器的倒计时心跳
  * @note   中断频率就等于当前音调（几百~几千Hz），而且只在响的时候开中断源，
  *         静音时一次都不进，CPU占用约0.2%。
  * @note   最后的TIM_ClearITPendingBit【故意不放进if里】。Key.c的中断是放里面的，
  *         这里不能照抄：TIM_GetITStatus要求SR和DIER同时有效才返回SET，而
  *         下面会调用Buzzer_Stop()把DIER.UIE清掉，一旦有个已挂起的IRQ进来，
  *         就走进不if，UIF会滞留在SR里，下次开中断时补一枪白响一声。
  */
void TIM3_IRQHandler(void)
{
	if (TIM_GetITStatus(BUZZER_TIM, TIM_IT_Update) == SET)
	{
		if (s_Remain > 0)
		{
			s_Remain --;

			if (s_Remain == 0)
			{
				Buzzer_Stop();				//本音响完了
			}
		}
	}

	TIM_ClearITPendingBit(BUZZER_TIM, TIM_IT_Update);
}

/*==================== 对外接口 ====================*/

/**
  * @brief  蜂鸣器初始化
  * @param  无
  * @retval 无
  * @note   必须在SystemClock_Config_72MHz()之后调用：PSC是按72MHz算的，
  *         在切时钟前跑的话音调会整体偏高9倍。
  *         初始化完【不启动定时器、不开中断源】，所以上电天然静音。
  */
void Buzzer_Init(void)
{
	GPIO_InitTypeDef GPIO_InitStructure;
	TIM_TimeBaseInitTypeDef TIM_TimeBaseStructure;
	TIM_OCInitTypeDef TIM_OCInitStructure;
	NVIC_InitTypeDef NVIC_InitStructure;

	/*开启时钟。这里【不需要】AFIO——GPIO_Mode_AF_PP写的是GPIOB->CRL，
	  不走AFIO->MAPR（AGENTS.md那条"AFIO必须先开"只针对GPIO_PinRemapConfig）。
	  项目里I2C2.c用GPIO_Mode_AF_OD也没开AFIO。*/
	RCC_APB2PeriphClockCmd(RCC_APB2Periph_GPIOB, ENABLE);	//开启GPIOB的时钟
	RCC_APB1PeriphClockCmd(RCC_APB1Periph_TIM3, ENABLE);	//开启TIM3的时钟（挂APB1，不是APB2）

	/*TIM3时基：1MHz计数时钟。ARR先给个1kHz的初值，每次Buzzer_Beep都会覆盖它*/
	TIM_InternalClockConfig(BUZZER_TIM);
	TIM_TimeBaseStructure.TIM_ClockDivision     = TIM_CKD_DIV1;
	TIM_TimeBaseStructure.TIM_CounterMode       = TIM_CounterMode_Up;
	TIM_TimeBaseStructure.TIM_Period            = (uint16_t)(BUZZER_TIMCLK_HZ / 1000 - 1);
	TIM_TimeBaseStructure.TIM_Prescaler         = BUZZER_TIM_PSC;
	TIM_TimeBaseStructure.TIM_RepetitionCounter = 0;	//TIM3没有RCR，此项被忽略
	TIM_TimeBaseInit(BUZZER_TIM, &TIM_TimeBaseStructure);

	/*TIM3通道4：PWM1模式，高电平有效。
	  ⚠ 本工程的TIM_OCInitTypeDef里【没有】TIM_OCPreload成员（那是F4/HAL才有的），
	    照抄网上代码会编译不过。
	  ⚠ TIM_OCPolarity必须显式赋值，理由见Buzzer_Load里的长注释。*/
	TIM_OCInitStructure.TIM_OCMode      = TIM_OCMode_PWM1;
	TIM_OCInitStructure.TIM_OutputState = TIM_OutputState_Enable;
	TIM_OCInitStructure.TIM_OCPolarity  = TIM_OCPolarity_High;	//=0x0000，即"不反相"
	TIM_OCInitStructure.TIM_Pulse       = 0;					//CCR=0 → PWM1下整周期都不活跃 → 输出恒低
	TIM_OC4Init(BUZZER_TIM, &TIM_OCInitStructure);

	/*TIM_TimeBaseInit内部会写EGR产生一次更新事件把UIF置起来，先清掉。
	  然后不启动定时器、不开中断源：上电静音，静音期间一次中断都不进*/
	TIM_ClearITPendingBit(BUZZER_TIM, TIM_IT_Update);
	TIM_ITConfig(BUZZER_TIM, TIM_IT_Update, DISABLE);
	TIM_Cmd(BUZZER_TIM, DISABLE);

	/*NVIC。注意 NVIC_PriorityGroupConfig 不在这里调——它是全局设置，放main()里。
	  抢占3/子3：数值大于Key.c里TIM2的2/2，也就是【优先级更低】。这样按键扫描
	  可以抢占蜂鸣器中断，反过来不行，Key.c那个已上板验证的10ms心跳时序零影响。*/
	NVIC_InitStructure.NVIC_IRQChannel                   = TIM3_IRQn;
	NVIC_InitStructure.NVIC_IRQChannelPreemptionPriority = 3;
	NVIC_InitStructure.NVIC_IRQChannelSubPriority        = 3;
	NVIC_InitStructure.NVIC_IRQChannelCmd                = ENABLE;
	NVIC_Init(&NVIC_InitStructure);

	/*最后才把PB1交给定时器。此刻定时器已经在驱动一个确定的低电平
	  （CC4E=1、CCR=0、CEN=0），切换的那一瞬间不会出现"输出已使能但没有
	  外设驱动它"的不确定态，也就不会有上电毛刺。

	  逐引脚调用，不用合并掩码——GPIO_Init会重写掩码里每一位的CNF/MODE，
	  一个手滑写成GPIO_Pin_All就会毁掉PB10/PB11(I2C2)、PB12(LED)、
	  甚至PA13/PA14(SWD)。*/
	GPIO_InitStructure.GPIO_Mode  = GPIO_Mode_AF_PP;
	GPIO_InitStructure.GPIO_Pin   = GPIO_Pin_1;
	GPIO_InitStructure.GPIO_Speed = GPIO_Speed_50MHz;
	GPIO_Init(GPIOB, &GPIO_InitStructure);
}

/**
  * @brief  响一声（非阻塞）
  * @param  FreqHz 音调频率，超出100~10000Hz会被静默夹取
  * @param  Ms 持续时间，超出1~5000ms会被静默夹取
  * @retval 无
  * @note   只写寄存器就返回，倒计时由TIM3更新中断完成，绝不阻塞主循环。
  *         正在响的时候再调一次=重新起音。
  */
void Buzzer_Beep(uint16_t FreqHz, uint16_t Ms)
{
	uint32_t Primask = __get_PRIMASK();

	/*整个配置序列关中断（约4µs）。理由有两层：
	  ① ISR里的 --s_Remain 是读-改-写，和Buzzer_Load里的赋值会竞争，
	     撞上就会把刚起的一声按掉；
	  ② 上一次响的"停机动作"可能正好落在本序列【之后】，把刚配好的音关掉。
	  关掉之后本序列不可能被切开，两个问题一起消失。
	  4µs对Key.c的10ms扫描周期是0.04%的抖动，不构成干扰。*/
	__disable_irq();

	/*先停计数器、再关中断源。反过来不行：计数器还在跑的时候关掉中断源，
	  更新事件会一直产生，虽然不响应中断但UIF会反复置起*/
	TIM_Cmd(BUZZER_TIM, DISABLE);
	TIM_ITConfig(BUZZER_TIM, TIM_IT_Update, DISABLE);

	Buzzer_Load(FreqHz, Ms);

	/*必须做：UG把CNT清零。不做的话CNT从上一轮冻结的值继续数，
	  新ARR比它小的时候（例如上一轮停在1800，这一轮ARR=954）计数器要一路
	  数到0xFFFF才回绕 → 第一声变成65ms的怪音，倒计时也少一拍*/
	TIM_GenerateEvent(BUZZER_TIM, TIM_EventSource_Update);

	/*必须做：UG会把UIF置起来。不先清掉的话，下面一开中断源NVIC线立刻有效，
	  中断会在配置还没结束时就冲进来*/
	TIM_ClearITPendingBit(BUZZER_TIM, TIM_IT_Update);

	TIM_ITConfig(BUZZER_TIM, TIM_IT_Update, ENABLE);
	TIM_Cmd(BUZZER_TIM, ENABLE);			//全配好了才启动

	__set_PRIMASK(Primask);
}

/**
  * @brief  立即停音
  * @param  无
  * @retval 无
  * @note   关键在第一步"强制无效"。F1的PWM输出电平不是简单的 CNT<CCR 比较器，
  *         而是被"更新事件置高、比较匹配拉低"驱动的，所以【把CCR改成0并不能
  *         立刻把已经置高的输出收回来】——而本模块的停机动作恰好发生在更新中断
  *         里（那一刻硬件刚把输出置高）。若只改CCR就TIM_Cmd(DISABLE)，会把引脚
  *         连同计数器一起冻结在高电平上，蜂鸣器持续通直流发烫，且是100%复现。
  *         TIM_ForcedOC4Config走的是OC4M="强制无效"，由输出级无条件拉低，与
  *         CNT/CCR/上述两种硬件模型都无关，是确定性的一步到位。
  *         代价是OC4M被改掉了，所以Buzzer_Load每次起音都要重配回PWM1模式。
  */
void Buzzer_Stop(void)
{
	TIM_ForcedOC4Config(BUZZER_TIM, TIM_ForcedAction_InActive);	//输出级无条件拉低

	TIM_Cmd(BUZZER_TIM, DISABLE);								//再停计数器，把低电平冻住
	TIM_ITConfig(BUZZER_TIM, TIM_IT_Update, DISABLE);

	s_Remain = 0;
}

/**
  * @brief  是否正在响
  * @param  无
  * @retval 1=正在响，0=静音
  */
uint8_t Buzzer_IsBusy(void)
{
	return (s_Remain > 0) ? 1 : 0;
}
