# 24xx EEPROM library for stm32 HAL
---  
## Please Do not Forget to get STAR, DONATE and support me on social networks. Thank you. :sparkling_heart:  
---   
-  Author:     Nima Askari  
-  Github:     https://www.github.com/NimaLTD
-  Youtube:    https://www.youtube.com/@nimaltd  
-  LinkedIn:   https://www.linkedin.com/in/nimaltd  
-  Instagram:  https://instagram.com/github.NimaLTD
---
* Install Library from https://github.com/nimaltd/STM32-PACK/raw/main/EE24/NimaLTD.I-CUBE-EE24.pdsc
* Add and enable it.
* Enable I2C.
* Select 'Generate peripheral initialization as a pair of .c/.h files per peripheral' on the Code Generator Tab.
* Generate code.
* Define a structure of `EE24_HandleTypeDef`.
* Call `EE24_Init()` and enjoy.
---
solve F1 i2c problem
```

void HAL_I2C_MspInit(I2C_HandleTypeDef* i2cHandle)
{

  GPIO_InitTypeDef GPIO_InitStruct = {0};
  if(i2cHandle->Instance==I2C2)
  {
  /* USER CODE BEGIN I2C2_MspInit 0 */
   __HAL_RCC_I2C2_CLK_ENABLE();  // <<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<  add manualy
  /* USER CODE END I2C2_MspInit 0 */
  
    __HAL_RCC_GPIOB_CLK_ENABLE();
    /**I2C2 GPIO Configuration    
    PB10     ------> I2C2_SCL
    PB11     ------> I2C2_SDA 
    */
    GPIO_InitStruct.Pin = GPIO_PIN_10|GPIO_PIN_11;
    GPIO_InitStruct.Mode = GPIO_MODE_AF_OD;
    GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_HIGH;
    HAL_GPIO_Init(GPIOB, &GPIO_InitStruct);

    /* I2C2 clock enable */
    __HAL_RCC_I2C2_CLK_ENABLE();
  /* USER CODE BEGIN I2C2_MspInit 1 */

  /* USER CODE END I2C2_MspInit 1 */
  }
}
```
example:
```
#include "ee24.h"

EE24_HandleTypeDef ee24;
uint8_t data[1024];
int main(void)
{
  ...
  ...
  ...
  if (EE24_Init(&ee24, &hi2c1, EE24_ADDRESS_DEFAULT))
  {
    EE24_Read(&ee24, 0, data, 1024, 1000);
  }
  while(1)
  {
  
  }
}
```
