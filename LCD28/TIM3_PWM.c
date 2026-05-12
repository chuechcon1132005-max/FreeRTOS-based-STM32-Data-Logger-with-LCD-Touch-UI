/*********************************************************************************************************
*
* File                : TIM3_PWM.c
* Hardware Environment: 
* Build Environment   : RealView MDK-ARM  Version: 4.20
* Version             : V1.0
* By                  : 
*
*                                  (c) Copyright 2005-2011, WaveShare
*                                       http://www.waveshare.net
*                                          All Rights Reserved
*
*********************************************************************************************************/

/* Includes ------------------------------------------------------------------*/
#include "TIM3_PWM.h"

/* Private define ------------------------------------------------------------*/


/* Private variables ---------------------------------------------------------*/


void TIM_Config(void)
{
	/*
	 * Legacy Waveshare backlight PWM code depended on STM32F2 StdPeriph API.
	 * This STM32F405 HAL project does not call TIM_Config(), so keep a no-op
	 * implementation to preserve linkage without pulling incompatible headers.
	 */
}

