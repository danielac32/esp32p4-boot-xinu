/******************************************************************************************
  Filename    : main.c
  
  Core        : RISC-V
  
  MCU         : ESP32-P4
    
  Author      : Chalandi Amine
 
  Owner       : Chalandi Amine
  
  Date        : 25.01.2026
  
  Description : Application main function for ULP-RISC-V Co-processor
  
                printf is resolved to the ROM routine at 0x4FC00024 by ulp.ld.
  
******************************************************************************************/

#include <stdio.h>

//-----------------------------------------------------------------------------------------
/// \brief  main function
///
/// \param  void
///
/// \return void
//-----------------------------------------------------------------------------------------
void main(void)
{
  //printf("BareMetal ESP32-P4 SW is Alive on LP core!\n\r");

  while(1);
}
