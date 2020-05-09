#ifndef	_EE24CONFIG_H
#define	_EE24CONFIG_H

#define		_EEPROM_SIZE_KBIT							64
#define		_EEPROM_I2C   								hi2c2			
#define		_EEPROM_USE_FREERTOS          1
#define		_EEPROM_ADDRESS               0xA0
#define		_EEPROM_USE_WP_PIN            0

#if (_EEPROM_USE_WP_PIN==1)
#define		_EEPROM_WP_GPIO								EE_WP_GPIO_Port
#define		_EEPROM_WP_PIN								EE_WP_Pin
#endif

#endif

