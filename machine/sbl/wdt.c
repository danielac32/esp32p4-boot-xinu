/******************************************************************************************
  Filename    : wdt.c
  
  Core        : RISC-V
  
  MCU         : ESP32-P4
    
  Author      : Chalandi Amine
 
  Owner       : Chalandi Amine
  
  Date        : 25.01.2026
  
  Description : Disable all Watchdogs
  
******************************************************************************************/
#include "esp32p4.h"

void wdt_disable_all_wdt(void);

//-----------------------------------------------------------------------------------------
/// \brief  
///
/// \param  
///
/// \return 
//-----------------------------------------------------------------------------------------
void wdt_disable_all_wdt(void)
{
  LP_WDT->WPROTECT.reg = 0x50D83AA1;
  LP_WDT->CONFIG0.reg  = 0;
  LP_WDT->WPROTECT.reg = 0;

  LP_WDT->SWD_WPROTECT.reg = 0x50D83AA1;
  LP_WDT->SWD_CONFIG.reg  |= (1ul<<30);
  LP_WDT->SWD_WPROTECT.reg = 0;

  TIMG0->WDTWPROTECT.reg = 0x50D83AA1;
  TIMG0->WDTCONFIG0.reg  = 0;
  TIMG0->WDTWPROTECT.reg = 0;

  TIMG1->WDTWPROTECT.reg = 0x50D83AA1;
  TIMG1->WDTCONFIG0.reg  = 0;
  TIMG1->WDTWPROTECT.reg = 0;
}
