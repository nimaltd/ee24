#ifndef _EE24_H_
#define _EE24_H_

/***********************************************************************************************************

  Author:     Nima Askari
  Github:     https://www.github.com/NimaLTD
  LinkedIn:   https://www.linkedin.com/in/nimaltd
  Youtube:    https://www.youtube.com/@nimaltd
  Instagram:  https://instagram.com/github.NimaLTD

  Version:    3.0.0

  History:
              3.0.0
              - Rewrite again
              - Support STM32CubeMx Packet installer

***********************************************************************************************************/

#ifdef __cplusplus
extern "C"
{
#endif

#include <stdbool.h>
#include "NimaLTD.I-CUBE-EE24_conf.h"
#include "i2c.h"

#if EE24_CMSIS_RTOS == EE24_CMSIS_RTOS_DISABLE
#define EE24_Delay(x) HAL_Delay(x)
#elif EE24_CMSIS_RTOS == EE24_CMSIS_RTOS_V1
#include "cmsis_os.h"
#define EE24_Delay(x) osDelay(x)
#else
#include "cmsis_os2.h"
#define EE24_Delay(x) osDelay(x)
#endif

#define EE24_ADDRESS_DEFAULT 0xA0

/***********************************************************************************************************/
/***********************************************************************************************************/
/***********************************************************************************************************/

typedef struct
{
	I2C_HandleTypeDef      *HI2c;
	uint8_t                Address;
	uint8_t                Lock;

} EE24_HandleTypeDef;

/***********************************************************************************************************/
/***********************************************************************************************************/
/***********************************************************************************************************/

bool EE24_Init(EE24_HandleTypeDef *Handle, I2C_HandleTypeDef *HI2c, uint8_t I2CAddress);
bool EE24_Read(EE24_HandleTypeDef *Handle, uint32_t Address, uint8_t *Data, size_t Len, uint32_t Timeout);
bool EE24_Write(EE24_HandleTypeDef *Handle, uint32_t Address, uint8_t *Data, size_t Len, uint32_t Timeout);

#ifdef __cplusplus
}
#endif
#endif
