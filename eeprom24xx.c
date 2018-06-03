
#include "i2c.h"
#include "eeprom24xx.h"
#if (_EEPROM_FREERTOS_IS_ENABLE==1)
#include "cmsis_os.h"
#endif
//##########################################################################
bool		EEPROM24XX_IsConnected(void)
{
	#if	(_EEPROM_USE_WP_PIN==1)
	HAL_GPIO_WritePin(_EEPROM_WP_GPIO,_EEPROM_WP_PIN,GPIO_PIN_SET);
	#endif
	if(HAL_I2C_IsDeviceReady(&_EEPROM24XX_I2C,0xa0,1,100)==HAL_OK)
		return true;
	else
		return false;	
}
//##########################################################################
bool		EEPROM24XX_Save(uint16_t Address,void *data,size_t size_of_data)
{
	#if ((_EEPROM_SIZE_KBIT==1) || (_EEPROM_SIZE_KBIT==2))
	if(size_of_data > 8)
		return false;
	#endif
	#if ((_EEPROM_SIZE_KBIT==4) || (_EEPROM_SIZE_KBIT==8) || (_EEPROM_SIZE_KBIT==16))
	if(size_of_data > 16)
		return false;
	#endif
	#if ((_EEPROM_SIZE_KBIT==32) || (_EEPROM_SIZE_KBIT==64) || (_EEPROM_SIZE_KBIT==128))
	if(size_of_data > 32)
		return false;
	#endif
	
	#if	(_EEPROM_USE_WP_PIN==1)
	HAL_GPIO_WritePin(_EEPROM_WP_GPIO,_EEPROM_WP_PIN,GPIO_PIN_RESET);
	#endif
	
	#if ((_EEPROM_SIZE_KBIT==1) || (_EEPROM_SIZE_KBIT==2))
	if(HAL_I2C_Mem_Write(&_EEPROM24XX_I2C,0xa0,Address,I2C_MEMADD_SIZE_8BIT,(uint8_t*)data,size_of_data,100) == HAL_OK)
	#else
	if(HAL_I2C_Mem_Write(&_EEPROM24XX_I2C,0xa0,Address,I2C_MEMADD_SIZE_16BIT,(uint8_t*)data,size_of_data,100) == HAL_OK)
	#endif
	{
		#if (_EEPROM_FREERTOS_IS_ENABLE==1)
		osDelay(7);
		#else
		HAL_Delay(7);
		#endif
		#if	(_EEPROM_USE_WP_PIN==1)
		HAL_GPIO_WritePin(_EEPROM_WP_GPIO,_EEPROM_WP_PIN,GPIO_PIN_SET);
		#endif
		return true;
	}
	else
	{
		#if	(_EEPROM_USE_WP_PIN==1)
		HAL_GPIO_WritePin(_EEPROM_WP_GPIO,_EEPROM_WP_PIN,GPIO_PIN_SET);
		#endif
		return false;		
	}
}
//##########################################################################
bool		EEPROM24XX_Load(uint16_t Address,void *data,size_t size_of_data)
{
	#if	(_EEPROM_USE_WP_PIN==1)
	HAL_GPIO_WritePin(_EEPROM_WP_GPIO,_EEPROM_WP_PIN,GPIO_PIN_SET);
	#endif
	#if ((_EEPROM_SIZE_KBIT==1) || (_EEPROM_SIZE_KBIT==2))
	if(HAL_I2C_Mem_Read(&_EEPROM24XX_I2C,0xa0,Address,I2C_MEMADD_SIZE_8BIT,(uint8_t*)data,size_of_data,100) == HAL_OK)
	#else
	if(HAL_I2C_Mem_Read(&_EEPROM24XX_I2C,0xa0,Address,I2C_MEMADD_SIZE_16BIT,(uint8_t*)data,size_of_data,100) == HAL_OK)
	#endif
	{
		return true;
	}
	else
		return false;		
}
