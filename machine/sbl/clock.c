/******************************************************************************************
  Filename    : clock.c
  
  Core        : RISC-V
  
  MCU         : ESP32-P4
    
  Author      : Chalandi Amine
 
  Owner       : Chalandi Amine
  
  Date        : 25.01.2026
  
  Description : Initialize and configure the clock tree
  
******************************************************************************************/

#include "esp32p4.h"

#define MHZ  1000000ul

extern int ets_printf(const char *fmt, ...);
extern uint32_t ets_clk_get_xtal_freq(void);
extern uint32_t ets_clk_get_cpu_freq(void);
void clk_info(void);
void clock_init(void);

//-----------------------------------------------------------------------------------------
/// \brief  
///
/// \param  
///
/// \return 
//-----------------------------------------------------------------------------------------
void clk_info(void)
{
  ets_printf("XTAL_FREQ = %u MHz\n\r" ,ets_clk_get_xtal_freq()/MHZ);
  ets_printf("CPU_FREQ  = %u MHz\n\r" ,ets_clk_get_cpu_freq()/MHZ);
}

//-----------------------------------------------------------------------------------------
/// \brief  
///
/// \param  
///
/// \return 
//-----------------------------------------------------------------------------------------
void clock_init(void)
{
  /* use CPLL_CLK as ROOT_CLK instead of the default XTAL (40MHz):
     - CPU_CLK (400MHz)
     - MEM_CLK (200MHz)
     - SYS_CLK (200MHz)
     - APB_CLK (100MHz)
  */
  LP_AON_CLKRST->LP_AONCLKRST_HP_CLK_CTRL.bit.LP_AONCLKRST_HP_ROOT_CLK_SRC_SEL = 1;

  /* print clock info */
  clk_info();
}