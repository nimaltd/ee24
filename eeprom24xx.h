#ifndef	_EEPROM_24XX_H
#define	_EEPROM_24XX_H

// 	eeprom 24cxxx library				version 1V01
//	"Nima Askari"								www.github.com/NimaLTD

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "eeprom24xxConfig.h"

bool		EEPROM24XX_IsConnected(void);
bool		EEPROM24XX_Save(uint16_t Address,void *data,size_t size_of_data);	
bool		EEPROM24XX_Load(uint16_t Address,void *data,size_t size_of_data);





#endif
