#ifndef	_24XXCONFIG_H
#define	_24XXCONFIG_H


#define		_EEPROM_SIZE_KBIT										32
#define		_EEPROM24XX_I2C											hi2c1			
#define		_EEPROM_FREERTOS_IS_ENABLE					1
#define		_EEPROM_USE_WP_PIN									1

#if (_EEPROM_USE_WP_PIN==1)
#define		_EEPROM_WP_GPIO											EE_WP_GPIO_Port
#define		_EEPROM_WP_PIN											EE_WP_Pin
#endif

#endif

