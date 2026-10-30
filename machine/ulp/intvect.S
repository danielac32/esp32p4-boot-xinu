/******************************************************************************************
  Filename    : intvect.S
  
  Core        : RISC-V
  
  MCU         : ESP32-P4
    
  Author      : Chalandi Amine
 
  Owner       : Chalandi Amine
  
  Date        : 27.01.2026
  
  Description : interrupt vector table implementation for ULP-RISC-V Co-processor
  
******************************************************************************************/


/*******************************************************************************************
  \brief  
  
  \param  
  
  \return 
********************************************************************************************/
.section .text
.type InterruptVectorTable, @function
.align 8
.globl InterruptVectorTable

.weak LP_UART_INTR
.weak LP_SW_INTR
.weak LP_TRNG_INTR
.weak LP_SPI_INTR
.weak LP_I2C_INTR
.weak LP_GPIO_INTR
.weak LP_ADC_INTR
.weak LP_TOUCH_INTR
.weak LP_TSENS_INTR
.weak LP_EFUSE_INTR
.weak LP_SYSREG_INTR
.weak LP_ANAPERI_INTR
.weak PMU_REG_x_INTR
.weak MB_xP_INTR
.weak LP_TIMER_REG_x_INTR
.weak LP_WDT_INTR
.weak LP_RTC_INTR
.weak HP_INTR

.set LP_UART_INTR       ,UndefinedHandler
.set LP_SW_INTR         ,UndefinedHandler
.set LP_TRNG_INTR       ,UndefinedHandler
.set LP_SPI_INTR        ,UndefinedHandler
.set LP_I2C_INTR        ,UndefinedHandler
.set LP_GPIO_INTR       ,UndefinedHandler
.set LP_ADC_INTR        ,UndefinedHandler
.set LP_TOUCH_INTR      ,UndefinedHandler
.set LP_TSENS_INTR      ,UndefinedHandler
.set LP_EFUSE_INTR      ,UndefinedHandler
.set LP_SYSREG_INTR     ,UndefinedHandler
.set LP_ANAPERI_INTR    ,UndefinedHandler
.set PMU_REG_x_INTR     ,UndefinedHandler
.set MB_xP_INTR         ,UndefinedHandler
.set LP_TIMER_REG_x_INTR,UndefinedHandler
.set LP_WDT_INTR        ,UndefinedHandler
.set LP_RTC_INTR        ,UndefinedHandler
.set HP_INTR            ,UndefinedHandler



InterruptVectorTable:
                      j ExceptionHandler      /* IRQ 00 */
                      j UndefinedHandler      /* IRQ 01 */
                      j UndefinedHandler      /* IRQ 02 */
                      j LP_SW_INTR            /* IRQ 03 */
                      j UndefinedHandler      /* IRQ 04 */
                      j UndefinedHandler      /* IRQ 05 */
                      j UndefinedHandler      /* IRQ 06 */
                      j LP_UART_INTR          /* IRQ 07 */
                      j UndefinedHandler      /* IRQ 08 */
                      j UndefinedHandler      /* IRQ 09 */
                      j UndefinedHandler      /* IRQ 10 */
                      j LP_SPI_INTR           /* IRQ 11 */
                      j UndefinedHandler      /* IRQ 12 */
                      j UndefinedHandler      /* IRQ 13 */
                      j UndefinedHandler      /* IRQ 14 */
                      j UndefinedHandler      /* IRQ 15 */
                      j LP_TRNG_INTR          /* IRQ 16 */
                      j LP_I2C_INTR           /* IRQ 17 */
                      j LP_GPIO_INTR          /* IRQ 18 */
                      j LP_ADC_INTR           /* IRQ 19 */
                      j LP_TOUCH_INTR         /* IRQ 20 */
                      j LP_TSENS_INTR         /* IRQ 21 */
                      j LP_EFUSE_INTR         /* IRQ 22 */
                      j LP_SYSREG_INTR        /* IRQ 23 */
                      j LP_ANAPERI_INTR       /* IRQ 24 */
                      j PMU_REG_x_INTR        /* IRQ 25 */
                      j MB_xP_INTR            /* IRQ 26 */
                      j LP_TIMER_REG_x_INTR   /* IRQ 27 */
                      j LP_WDT_INTR           /* IRQ 28 */
                      j LP_RTC_INTR           /* IRQ 29 */
                      j HP_INTR               /* IRQ 30 */

ExceptionHandler:
                  j ExceptionHandler


UndefinedHandler:
                  j UndefinedHandler

.size InterruptVectorTable, .-InterruptVectorTable
