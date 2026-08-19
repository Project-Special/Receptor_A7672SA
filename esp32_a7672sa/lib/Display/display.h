// Display 4" ILI9488 SPI 480x320.
//
// Pinos vêm por build flag no platformio.ini (mesmo pinout do projeto
// IHM_esp32), e nenhum deles colide com o A7672: a UART do módulo fica em
// GPIO16/17 e o PWRKEY em GPIO4, fora do barramento SPI do display.
#pragma once

#include <TFT_eSPI.h>

extern TFT_eSPI tft;
