

#include "ee24.h"

#if (EE24_SIZE <= 2)
#define EE24_PSIZE     8
#elif (EE24_SIZE <= 16)
#define EE24_PSIZE     16
#else
#define EE24_PSIZE     32
#endif

/***********************************************************************************************************/

void EE24_Lock(EE24_HandleTypeDef *Handle)
{
	while (Handle->Lock)
	{
		EE24_Delay(1);
	}
	Handle->Lock = 1;
}

/***********************************************************************************************************/

void EE24_UnLock(EE24_HandleTypeDef *Handle)
{
	Handle->Lock = 0;
}

/***********************************************************************************************************/
/***********************************************************************************************************/
/***********************************************************************************************************/

bool EE24_Init(EE24_HandleTypeDef *Handle, I2C_HandleTypeDef *HI2c, uint8_t I2CAddress)
{
	bool answer = false;
	do
	{
		if ((Handle == NULL) || (HI2c == NULL))
		{
			break;
		}
		Handle->HI2c = HI2c;
		Handle->Address = I2CAddress;
		if (HAL_I2C_IsDeviceReady(Handle->HI2c, Handle->Address, 2, 100) == HAL_OK)
		{
			answer = true;
		}
	}
	while (0);

	return answer;
}

/***********************************************************************************************************/

bool EE24_Read(EE24_HandleTypeDef *Handle, uint32_t Address, uint8_t *Data, size_t Len, uint32_t Timeout)
{
	EE24_Lock(Handle);
	bool answer = false;
	do
	{
#if ((EE24_SIZE == EE24_1KBIT) || (EE24_SIZE == EE24_2KBIT))
    if (HAL_I2C_Mem_Read(Handle->HI2c, Handle->Address, Address, I2C_MEMADD_SIZE_8BIT, Data, Len, Timeout) == HAL_OK)
#elif (EE24_SIZE == EE24_4KBIT)
    if (HAL_I2C_Mem_Read(Handle->HI2c, Handle->Address | ((Address & 0x0100) >> 7), (Address & 0xff), I2C_MEMADD_SIZE_8BIT, Data, Len, Timeout) == HAL_OK)
#elif (EE24_SIZE == EE24_8KBIT)
    if (HAL_I2C_Mem_Read(Handle->HI2c, Handle->Address | ((Address & 0x0300) >> 7), (Address & 0xff), I2C_MEMADD_SIZE_8BIT, Data, Len, Timeout) == HAL_OK)
#elif (EE24_SIZE == EE24_16KBIT)
	  if (HAL_I2C_Mem_Read(Handle->HI2c, Handle->Address | ((Address & 0x0700) >> 7), (Address & 0xff), I2C_MEMADD_SIZE_8BIT, Data, Len, Timeout) == HAL_OK)
#else
    if (HAL_I2C_Mem_Read(Handle->HI2c, Handle->Address, Address, I2C_MEMADD_SIZE_16BIT, Data, Len, Timeout) == HAL_OK)
#endif
    {
    	answer = true;
    }
	}
	while (0);

	EE24_UnLock(Handle);
	return answer;
}

/***********************************************************************************************************/

bool EE24_Write(EE24_HandleTypeDef *Handle, uint32_t Address, uint8_t *Data, size_t Len, uint32_t Timeout)
{
	EE24_Lock(Handle);
	bool answer = false;
	do
	{
	  uint16_t w;
	  uint32_t startTime = HAL_GetTick();
	  while (1)
	  {
	    w = EE24_PSIZE - (Address  % EE24_PSIZE);
	    if (w > Len)
	    {
	    	w = Len;
	    }
#if ((EE24_SIZE == EE24_1KBIT) || (EE24_SIZE == EE24_2KBIT))
        if (HAL_I2C_Mem_Write(Handle->HI2c, Handle->Address, Address, I2C_MEMADD_SIZE_8BIT, Data, Len, Timeout) == HAL_OK)
#elif (EE24_SIZE == EE24_4KBIT)
        if (HAL_I2C_Mem_Write(Handle->HI2c, Handle->Address | ((Address & 0x0100) >> 7), (Address & 0xff), I2C_MEMADD_SIZE_8BIT, Data, w, Timeout) == HAL_OK)
#elif (EE24_SIZE == EE24_8KBIT)
        if (HAL_I2C_Mem_Write(Handle->HI2c, Handle->Address | ((Address & 0x0300) >> 7), (Address & 0xff), I2C_MEMADD_SIZE_8BIT, Data, w, Timeout) == HAL_OK)
#elif (EE24_SIZE == EE24_16KBIT)
        if (HAL_I2C_Mem_Write(Handle->HI2c, Handle->Address | ((Address & 0x0700) >> 7), (Address & 0xff), I2C_MEMADD_SIZE_8BIT, Data, w, Timeout) == HAL_OK)
#else
        if (HAL_I2C_Mem_Write(Handle->HI2c, Handle->Address, Address, I2C_MEMADD_SIZE_16BIT, Data, w, Timeout) == HAL_OK)
#endif
	    {
	      EE24_Delay(10);
	      Len -= w;
	      Data += w;
	      Address += w;
	      if (Len == 0)
	      {
	        answer = true;
	        break;
	      }
	      if (HAL_GetTick() - startTime >= Timeout)
	      {
	        break;
	      }
	    }
	    else
	    {
	    	break;
	    }
	  }
	}
	while (0);

	EE24_UnLock(Handle);
	return answer;
}

/***********************************************************************************************************/
