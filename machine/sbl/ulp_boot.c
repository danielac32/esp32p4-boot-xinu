/******************************************************************************************
  Filename    : ulp.c
  
  Core        : RISC-V
  
  MCU         : ESP32-P4
    
  Author      : Chalandi Amine
 
  Owner       : Chalandi Amine
  
  Date        : 25.01.2026
  
  Description : Initialize and boot the ULP core (ULP-RISC-V)
  
******************************************************************************************/

#include <stdint.h>
#include "esp32p4.h"

void ulp_boot(void);

//-----------------------------------------------------------------------------------------
/// \brief  
///
/// \param  
///
/// \return 
//-----------------------------------------------------------------------------------------
void ulp_boot(void)
{
  extern uint32_t __ulp_entry_point;
  #define ULP_ENTRY_POINT_REG (*(volatile uint32_t*)(0x50110028))
  
  /* configure the reset handler of LP_CPU */
  ULP_ENTRY_POINT_REG = (uint32_t)&__ulp_entry_point;

  /* enable reset and stall in sleep mode */
  PMU->LP_CPU_PWR0.bit.LP_CPU_SLP_STALL_EN = 1;
  PMU->LP_CPU_PWR0.bit.LP_CPU_SLP_RESET_EN = 1;

  /* configure LP CPU to enter sleep mode after reset */
  PMU->LP_CPU_PWR1.bit.LP_CPU_SLEEP_REQ = 1;

  /* set the wakeup source to be software wakeup triggered from a HP core */
  PMU->LP_CPU_PWR2.bit.LP_CPU_WAKEUP_EN = (1UL << 22); 

  /* trigger software reset on LP CPU (it will restart in sleep mode) */
  LP_PERI->RESET_EN.bit.RST_EN_LP_CORE = 1;

  /* set FAST_CLK clock (clock for the LP CPU Active state) to 40MHz */
  LP_AON_CLKRST->LP_AONCLKRST_LP_CLK_CONF.bit.LP_AONCLKRST_FAST_CLK_SEL = 0;

  /* enable clock for LP CPU */
  LP_PERI->CLK_EN.bit.CK_EN_LP_CORE = 1;

  /* trigger a SW wakeup for current HP core */
  PMU->HP_LP_CPU_COMM.bit.HP_TRIGGER_LP = 1;

  /* wait till the LP CPU is stalled */
  while(!PMU->LP_CPU_PWR0.bit.LP_CPU_SLP_STALL_FLAG_EN);
    
  /* release the LP CPU from stall state */
  PMU->LP_CPU_PWR0.bit.LP_CPU_SLP_STALL_EN = 0;
}
