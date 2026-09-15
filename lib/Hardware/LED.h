#ifndef __LED_H
#define __LED_H

#include "stm32f10x.h"                  // Device header

/*本板LED是高电平点亮，接线：
    PB12 ──[1k]──|>|── GND
    正极长脚经1k电阻接PB12，负极短脚接GND

  注意这与江协教程的约定相反（那个是低电平点亮，LED_ON里写ResetBits），
  本驱动按实际接线写成高电平有效。*/

void LED_Init(void);
void LED_ON(void);
void LED_OFF(void);
void LED_Toggle(void);

#endif
