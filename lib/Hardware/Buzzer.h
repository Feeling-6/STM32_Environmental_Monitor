#ifndef __BUZZER_H
#define __BUZZER_H

#include "stm32f10x.h"                  // Device header

/*按键提示音用的音高（十二平均律，A4=440Hz）。
  只列了本项目用到的6个，要别的音按 f = 440 × 2^((n-69)/12) 自己算。*/
#define BUZZER_NOTE_C5		523
#define BUZZER_NOTE_D5		587
#define BUZZER_NOTE_E5		659
#define BUZZER_NOTE_G5		784
#define BUZZER_NOTE_A5		880
#define BUZZER_NOTE_C6		1047

/*初始化：配TIM3_CH4(PB1) + NVIC。必须在SystemClock_Config_72MHz()之后调用
  （PSC是按72MHz算的）。不启动定时器也不开中断源，所以上电天然静音。
  建议放在Key_Init()之前——虽然本模块不碰AFIO->MAPR，不受它的副作用影响。*/
void Buzzer_Init(void);

/*响一声（★非阻塞）：FreqHz赫兹响Ms毫秒，靠TIM3更新中断自己倒计时停机，
  只写寄存器就返回，绝不卡主循环。正在响的时候再调一次=重新起音，不是叠加。
  FreqHz和Ms超出安全范围会被静默夹取（100~10000Hz，1~5000ms）。*/
void Buzzer_Beep(uint16_t FreqHz, uint16_t Ms);

/*立即停音。主循环和中断里都会调它，两处都安全。
  停音后PB1如果是高电平，说明"强制无效"那条路没生效，见Buzzer.c里Stop的注释。*/
void Buzzer_Stop(void);

/*是否正在响。供调试界面显示，与AD_GetError/Key_GetDropped同类*/
uint8_t Buzzer_IsBusy(void);

/*---------------- 引脚占用提醒 ----------------
  PB1 = TIM3_CH4，走【默认映射】，全工程不调用任何GPIO_PinRemapConfig。
  这点是刻意的：Key_Init()会清掉AFIO->MAPR低24位（抹掉所有重映射），
  用默认映射天然躲开这个坑。

  ⚠ PB1 同时是 ADC1_IN9 —— 以后给ADC加通道时不能碰它（当前规则组只有IN0/IN1）
  ⚠ TIM3_CH2 = PB5 是【按键4】。本模块只开CC4E，绝不碰CCER的CC2E位、
    绝不调TIM_OC2Init，所以TIM3不会去驱动PB5
  ⚠ TIM3是通用定时器，【不需要】TIM_CtrlPWMOutputs（那是TIM1/TIM8的MOE位，
    只有高级定时器才有）*/

#endif
