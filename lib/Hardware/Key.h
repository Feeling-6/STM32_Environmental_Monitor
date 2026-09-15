#ifndef __KEY_H
#define __KEY_H

#include "stm32f10x.h"                  // Device header

/*按键编号：1~6，顺序与引脚分配表一致*/
#define KEY_COUNT			6

/*---------------- 时序参数（单位：扫描周期，1周期=10ms）----------------
  改这些宏就能调手感，不用动状态机逻辑*/
#define KEY_DEBOUNCE_TICKS	2		//消抖：连续N次采样一致才认可，20ms跨度/最坏30ms
#define KEY_LONG_TICKS		100		//长按判定：1000ms
#define KEY_REPEAT_TICKS	20		//长按重复间隔：200ms
#define KEY_REPEAT_MAX		50		//长按重复上限，约10秒。防卡键无限刷事件
#define KEY_DOUBLE_TICKS	40		//双击窗口：400ms
/*注意 KEY_DOUBLE_TICKS 是量在"消抖后的松开"到"第二击消抖后按下"之间，
  第二击自己还要20~30ms消抖，所以用户实际可用的窗口约370ms。
  别低于25，否则会明显漏双击；嫌单击响应慢可降到30，代价是双击判定变严。*/

/*事件类型。0保留为"无事件"*/
typedef enum
{
	KEY_EVENT_NONE = 0,
	KEY_EVENT_CLICK,			//单击
	KEY_EVENT_DOUBLE,			//双击
	KEY_EVENT_LONG,				//长按（按下满1s时报一次）
	KEY_EVENT_LONG_REPEAT,		//长按重复（之后每200ms一次，直到松开或到上限）
} KeyEventType;

/*按键事件*/
typedef struct
{
	uint8_t Key;				//按键编号 1~6
	uint8_t Event;				//KeyEventType
} KeyEvent;

/*初始化：配GPIO + 启动TIM2中断扫描。必须在SystemClock_Config_72MHz()之后调用。
  另外它内部会做SWJ重映射（释放PB3），会清掉AFIO->MAPR低24位，
  所以本函数要在main()里最后调用，且以后任何GPIO_PinRemapConfig都要放在它之后。*/
void Key_Init(void);

/*取事件。返回1=取到，0=队列空。主循环里while循环取干净即可*/
uint8_t Key_GetEvent(KeyEvent *Event);

/*实时按下状态（消抖后）。Key范围1~6，越界返回0*/
uint8_t Key_IsDown(uint8_t Key);

/*被丢弃的事件数，正常应恒为0（队列满了才会丢，说明主循环太久没取）*/
uint16_t Key_GetDropped(void);

/*事件名的可显示字符串，长度固定6字符（如"CLICK "/"DOUBLE"），供调试界面用*/
char *Key_EventName(uint8_t Event);

/*---------------- 引脚占用提醒 ----------------
  PB3  = TIM2_CH2 的默认复用脚 —— 本驱动没开TIM2通道所以无影响，
         但以后想用TIM2输出PWM得先把按键挪走
  PB12 = TIM1_BKIN，PA12 = TIM1_ETR —— TIM1目前没用，用到时要留意*/

#endif
