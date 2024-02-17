/**
  ******************************************************************************
  * File Name          : NimaLTD.I-CUBE-EE24_conf.h
  * Description        : This file provides code for the configuration
  *                      of the NimaLTD.I-CUBE-EE24_conf.h instances.
  ******************************************************************************
  * @attention
  *
  * Copyright (c) 2024 STMicroelectronics.
  * All rights reserved.
  *
  * This software is licensed under terms that can be found in the LICENSE file
  * in the root directory of this software component.
  * If no LICENSE file comes with this software, it is provided AS-IS.
  *
  ******************************************************************************
  */
/* Define to prevent recursive inclusion -------------------------------------*/
#ifndef _NIMALTD_I_CUBE_EE24_CONF_H_
#define _NIMALTD_I_CUBE_EE24_CONF_H_

#ifdef __cplusplus
 extern "C" {
#endif

#define EE24_1KBIT                            1
#define EE24_2KBIT                            2
#define EE24_4KBIT                            4
#define EE24_8KBIT                            8
#define EE24_16KBIT                           16
#define EE24_32KBIT                           32
#define EE24_64KBIT                           64
#define EE24_128KBIT                          128
#define EE24_256KBIT                          256
#define EE24_512KBIT                          512

#define EE24_CMSIS_RTOS_DISABLE               0
#define EE24_CMSIS_RTOS_V1                    1
#define EE24_CMSIS_RTOS_V2                    2

/**
	MiddleWare name : NimaLTD.I-CUBE-EE24.3.1.0
	MiddleWare fileName : NimaLTD.I-CUBE-EE24_conf.h
*/
/*---------- EE24_SIZE  -----------*/
#define EE24_SIZE      EE24_8KBIT

/*---------- EE24_CMSIS_RTOS  -----------*/
#define EE24_CMSIS_RTOS      EE24_CMSIS_RTOS_DISABLE

/*---------- EE24_USE_WP_PIN  -----------*/
#define EE24_USE_WP_PIN      true

#ifdef __cplusplus
}
#endif
#endif /* _NIMALTD_I_CUBE_EE24_CONF_H_ */

/**
  * @}
  */

/************************ (C) COPYRIGHT STMicroelectronics *****END OF FILE****/

