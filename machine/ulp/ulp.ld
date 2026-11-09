/******************************************************************************************
  Filename    : ulp.ld
  
  Core        : RISC-V
  
  MCU         : ESP32-P4
    
  Author      : Chalandi Amine
 
  Owner       : Chalandi Amine
  
  Date        : 24.02.2025
  
  Description : Linker description file script for ESP32-P4 co-processor (ULP-RISC-V)
  
                Portado del Coprocessor/ulp.ld de Chalandi.  El LP core empieza a
                ejecutar en el offset 0x400000 de la flash (XIP 0x40400000),
                dirección que le da ulp_boot() del SBL.  La ranura del ULP son
                SOLO 64 KB de flash (0x400000..0x410000), y su RAM son los
                32 KB EXACTOS del final del PSRAM: 0x49FF8000..0x4A000000.
                El offset era 0x100000 y se subio a 4 MB el 2026-11-04 para
                dejar al kernel un hueco contiguo de 0x5000..0x400000 (4 MB)
                antes de meter LVGL; desde aqui hasta el final de la flash de
                16 MB queda libre para el mapa de particiones (doc
                docs/port-esp32p4/07).  La pareja de este ORIGIN es
                __ulp_entry_point en machine/sbl/SBL.ld: si se mueve uno sin
                el otro, el LP salta a basura.
                (Chalandi usaba 0x8000, que con un kernel de ~348 KB quedaría
                pisado: sus 12 KB de ventana sólo servían a su demo diminuta.)
                El PSRAM ya está inicializado y mapeado por el SBL antes de soltar
                el LP core.  La frontera con el kernel la fijan dos cifras que
                tienen que cuadrar: aquí ORIGIN+32K = 0x4A000000, y en el kernel
                `RAM LENGTH = 32M-32K` (ld.script:27) da _psram_free_end =
                0x49FF8000.  Si alguien sube esa LENGTH a 32M, el heap del kernel
                se come la pila del LP en silencio.

******************************************************************************************/

/******************************************************************************************
 ELF Entrypoint
******************************************************************************************/
ENTRY(_start)

/******************************************************************************************
 Globals
******************************************************************************************/
__STACK_SIZE = 1K;

/******************************************************************************************
 Memory configuration
******************************************************************************************/

MEMORY
{
  FLASH(rx)     : ORIGIN = 0x40400000, LENGTH = 64K
  RAM(rwx)      : ORIGIN = 0x49FF8000, LENGTH = 32K
}

/******************************************************************************************
 Sections definition
******************************************************************************************/
SECTIONS
{
  /* Program code (text) */
  .text : ALIGN(4)
  {
    PROVIDE(__CODE_BASE_ADDRESS = .);
    *(.boot)
    *(.text)
    . = ALIGN(4);
  } > FLASH

  /* Read-only data (.rodata) */
  .rodata : ALIGN(4)
  {
    PROVIDE(__RODATA_BASE_ADDRESS = .);
    *(.rodata)
  } > FLASH

 /* Section for constructors */
  .ctors : ALIGN(4)
  {
    PROVIDE(__CTOR_LIST__  = .);
    KEEP (*(SORT(.ctors.*)))
    KEEP (*(.ctors))
    KEEP (*(SORT(.init_array.*)))
    KEEP (*(.init_array))
    LONG(-1) ;
    PROVIDE(__CTOR_END__  = .);
    . = ALIGN(4);
  }  > FLASH


  /* Section for destructors */
  .dtors : ALIGN(4)
  {
    PROVIDE(__DTOR_LIST__  = .);
    KEEP (*(SORT(.dtors.*)))
    KEEP (*(.dtors))
    KEEP (*(SORT(.fini_array.*)))
    KEEP (*(.fini_array))
    LONG(-1) ;
    PROVIDE(__DTOR_END__  = .);
    . = ALIGN(4);
  } > FLASH

  /* Runtime clear table */
  .clear_sec : ALIGN(4)
  {
    PROVIDE(__RUNTIME_CLEAR_TABLE = .) ;
    LONG(0 + ADDR(.bss));   LONG(SIZEOF(.bss));
    LONG(-1);                 LONG(-1);
    . = ALIGN(4);
  } > FLASH

  /* Runtime copy table */
  .copy_sec : ALIGN(4)
  {
    PROVIDE(__RUNTIME_COPY_TABLE = .) ;
    LONG(LOADADDR(.data));   LONG(0 + ADDR(.data));   LONG(SIZEOF(.data));
    LONG(-1);                LONG(-1);                LONG(-1);
    . = ALIGN(4);
    PROVIDE(__CODE_END_ADDRESS = .);
  } > FLASH 

  /* The ROM-to-RAM initialized data sections */
  .data : ALIGN(4) 
  {
    *(.data)
    *(.data*)
    . = ALIGN(4);
  } > RAM  AT>FLASH
  
  /* The uninitialized (zero-cleared) data sections */
  .bss : ALIGN(4)
  {
    *(.bss)
    *(.bss*)
    . = ALIGN(4);
  } > RAM

  /* stack definition (note: fixed vs Chalandi's original — his
     `. = ALIGN(MAX(__STACK_SIZE, .), 8)` resolved to a 0-byte stack) */
  .stack (NOLOAD) :
  {
    . = ALIGN(8);
    PROVIDE(__STACK_BOTTOM = .) ;
    . = . + __STACK_SIZE;
    PROVIDE(__STACK_TOP = .) ;
  } > RAM

  /* ROM APIs */
  printf = 0x4fc00024;

}
