#include "stm32f10x.h"                  // Device header
#include "AD.h"
#include "Delay.h"

/*ADC寄存器里用到但SPL v3.6.0没有导出的位。
  ADC_FLAG_* 只到 STRT，没有 OVR；而 ADC_GetFlagStatus/ADC_ClearFlag 内部有
  assert_param(IS_ADC_GET_FLAG(...))，传 0x20 在 USE_FULL_ASSERT 打开时会挂掉。
  所以这里直接操作寄存器，不依赖那个开关。*/
#define AD_ADC_SR_OVR	0x20u				//SR的bit5=OVR（规则组溢出），SR是写0清除寄存器

/*DMA搬运目标，硬件每11.33us刷新一次。
  volatile的原因见AD.h。这里特意不加static：按你的选择对外暴露，
  可以像教程那样直接用下标读。*/
volatile uint16_t AD_Value[2];

/**
  * @brief  ADC1双通道连续扫描 + DMA循环搬运 初始化
  * @param  无
  * @retval 无
  * @note   必须在SystemClock_Config_72MHz()之后调用：那个函数里的RCC_DeInit()
  *         会清掉RCC_ADCCLKConfig，ADC会退回默认的PCLK2/2=36MHz，远超14MHz上限。
  *
  *         初始化完成后ADC一直在转换、DMA一直在搬运，永不停止。
  *         绝不能在之后重启或重新配置ADC：CNDTR/CMAR在EN=1时写保护，
  *         关掉通道也不会复位，重启后ADC从rank1开始而DMA从上次槽位继续，
  *         会导致永久性的通道互换（烟雾值跑进光敏变量里）。
  *         真要重启，唯一安全配方是完整重来：
  *         ADC_Cmd(DISABLE) → DMA_Cmd(DISABLE) → DMA_DeInit → DMA_Init
  *         → ADC_Cmd(ENABLE) → 校准 → DMA_Cmd(ENABLE) → SWSTART
  */
void AD_Init(void)
{
	GPIO_InitTypeDef GPIO_InitStructure;
	ADC_InitTypeDef ADC_InitStructure;
	DMA_InitTypeDef DMA_InitStructure;

	/*开启时钟*/
	RCC_APB2PeriphClockCmd(RCC_APB2Periph_GPIOA, ENABLE);	//开启GPIOA的时钟
	RCC_APB2PeriphClockCmd(RCC_APB2Periph_ADC1, ENABLE);	//开启ADC1的时钟（挂在APB2）
	RCC_AHBPeriphClockCmd(RCC_AHBPeriph_DMA1, ENABLE);		//开启DMA1的时钟（挂在AHB，不是APB）

	RCC_ADCCLKConfig(RCC_PCLK2_Div6);						//ADC时钟 = 72MHz/6 = 12MHz（上限14MHz）

	/*GPIO初始化：模拟输入模式（不是复用）*/
	GPIO_InitStructure.GPIO_Mode  = GPIO_Mode_AIN;
	GPIO_InitStructure.GPIO_Pin   = GPIO_Pin_0 | GPIO_Pin_1;	//PA0=MQ-2，PA1=光敏
	GPIO_InitStructure.GPIO_Speed = GPIO_Speed_50MHz;
	GPIO_Init(GPIOA, &GPIO_InitStructure);

	/*ADC初始化：先定框架（CR1/CR2/SQR1的L位）*/
	ADC_InitStructure.ADC_Mode                = ADC_Mode_Independent;		//独立模式
	ADC_InitStructure.ADC_ScanConvMode        = ENABLE;						//扫描模式：多通道
	ADC_InitStructure.ADC_ContinuousConvMode  = ENABLE;						//连续转换：不停
	ADC_InitStructure.ADC_ExternalTrigConv    = ADC_ExternalTrigConv_None;	//不用外部触发
	ADC_InitStructure.ADC_DataAlign           = ADC_DataAlign_Right;		//右对齐
	ADC_InitStructure.ADC_NbrOfChannel        = 2;							//★必须等于下面排的rank数
	ADC_Init(ADC1, &ADC_InitStructure);

	/*再排通道（SQR3/SMPR）。扫描顺序由rank决定：
 	  rank1 → AD_Value[0]，rank2 → AD_Value[1]。
 	  这两步和ADC_Init互不干扰（写的寄存器零重叠），但按ST的顺序写更稳：
 	  万一以后有人加ADC_DeInit，反序写法会被静默清掉rank配置。*/
	ADC_RegularChannelConfig(ADC1, ADC_Channel_0, 1, ADC_SampleTime_55Cycles5);	//rank1 = PA0
	ADC_RegularChannelConfig(ADC1, ADC_Channel_1, 2, ADC_SampleTime_55Cycles5);	//rank2 = PA1

	/*DMA初始化
	  DMA1_Channel1是硬件固定的：ADC1的DMA请求只连到通道1，不能改*/
	DMA_DeInit(DMA1_Channel1);
	DMA_InitStructure.DMA_PeripheralBaseAddr = (uint32_t)&ADC1->DR;				//源：ADC数据寄存器，固定
	DMA_InitStructure.DMA_MemoryBaseAddr     = (uint32_t)&AD_Value[0];			//目标：数组
	DMA_InitStructure.DMA_DIR                = DMA_DIR_PeripheralSRC;			//方向：外设→存储器
	DMA_InitStructure.DMA_BufferSize         = 2;								//★必须 = ADC_NbrOfChannel
	DMA_InitStructure.DMA_PeripheralInc      = DMA_PeripheralInc_Disable;		//源地址不自增
	DMA_InitStructure.DMA_MemoryInc          = DMA_MemoryInc_Enable;			//★目标自增，漏了则两路都写[0]
	DMA_InitStructure.DMA_PeripheralDataSize = DMA_PeripheralDataSize_HalfWord;	//16位
	DMA_InitStructure.DMA_MemoryDataSize     = DMA_MemoryDataSize_HalfWord;		//16位
	DMA_InitStructure.DMA_Mode               = DMA_Mode_Circular;				//循环模式，永不停
	DMA_InitStructure.DMA_Priority           = DMA_Priority_High;
	DMA_InitStructure.DMA_M2M                = DMA_M2M_Disable;					//硬件触发（ADC）
	DMA_Init(DMA1_Channel1, &DMA_InitStructure);

	/*ADC上电，等tSTAB后再打开DMA请求*/
	ADC_Cmd(ADC1, ENABLE);
	Delay_us(2);											//F103要求ADON=1后约1us才能开始校准
	ADC_DMACmd(ADC1, ENABLE);								//允许ADC产生DMA请求

	/*校准：必须在ADON=1之后、启动转换之前*/
	ADC_ResetCalibration(ADC1);
	while (ADC_GetResetCalibrationStatus(ADC1) == SET);		//等复位校准完成
	ADC_StartCalibration(ADC1);
	while (ADC_GetCalibrationStatus(ADC1) == SET);			//等校准完成

	/*最后才武装DMA，然后点火。
	  把DMA放到这里是为了证明不存在"转换已开始而DMA未就绪"的窗口——
	  那个窗口会导致相位偏移，而相位一偏就是永久性通道互换。
	  ADC_SoftwareStartConvCmd整个程序只调这一次，之后连续转换永不停止
	  （注意它传DISABLE并不能停掉正在跑的连续转换，只有ADC_Cmd(DISABLE)能停）。*/
	DMA_Cmd(DMA1_Channel1, ENABLE);
	ADC_SoftwareStartConvCmd(ADC1, ENABLE);
}

/**
  * @brief  读取MQ-2的原始值
  * @param  无
  * @retval 0~4095
  */
uint16_t AD_GetMQ2(void)
{
	return AD_Value[AD_MQ2];
}

/**
  * @brief  读取光敏电阻的原始值
  * @param  无
  * @retval 0~4095
  */
uint16_t AD_GetLight(void)
{
	return AD_Value[AD_LIGHT];
}

/**
  * @brief  原始值换算成引脚电压
  * @param  Raw 原始值，0~4095
  * @retval 引脚电压，0~3.3V
  * @note   除数是4096不是4095：F103的传递函数是 Vin = raw × VDDA/4096，
  *         12位满量程是4096个刻度。教程用4095，差0.02%，但4096才是对的。
  */
float AD_GetVoltage(uint16_t Raw)
{
	return (float)Raw / 4096.0f * AD_VREF;
}

/**
  * @brief  折算回MQ-2的AO引脚电压（含分压还原）
  * @param  无
  * @retval AO引脚电压，0~5V（顶部约1%会削顶，见AD.h说明）
  */
float AD_GetMQ2Voltage(void)
{
	return AD_GetVoltage(AD_GetMQ2()) * AD_MQ2_DIVIDER;
}

/**
  * @brief  读取ADC/DMA错误标志
  * @param  无
  * @retval 0=正常；bit0=ADC溢出(OVR)，bit1=DMA传输错误(TE)
  * @note   读后自动清除（状态寄存器语义）。配合调试器Watch窗口使用。
  */
uint8_t AD_GetError(void)
{
	uint8_t err = 0;

	if (ADC1->SR & AD_ADC_SR_OVR)							//ADC溢出：DMA没及时取走数据
	{
		err |= 0x01;
		ADC1->SR = ~AD_ADC_SR_OVR;							//写0清除
	}

	if (DMA_GetFlagStatus(DMA1_FLAG_TE1) == SET)			//DMA传输错误
	{
		err |= 0x02;
		DMA_ClearFlag(DMA1_FLAG_TE1);
	}

	return err;
}
