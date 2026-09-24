/*
** p33AK512MPS506_noinit_ram_reserve.ld -- reserve the runtime no-init block in
** a standalone dsPIC33AK512MPS506 image.
**
** This is deliberately a supplementary script, used together with the DFP's
** default standalone linker script.  It reserves only .noinit_ram; it does not
** relocate vectors, reserve a resident region, or encode a delivery layout.
** The address is selected by shared/noinit_ram_config.h and is the top 0xD0
** bytes of the MPS506 data RAM (0x13F30..0x13FFF).
*/

SECTIONS
{
  /* KEEP is required with --gc-sections; NOLOAD preserves warm-reset state.
  ** The data-region binding makes the DFP script account for this range while
  ** placing the automatic stack. The DFP script is loaded later, so xc-dsc-ld
  ** reports a harmless region-declaration ordering warning; omitting the
  ** binding instead lets --gc-sections discard this critical reservation. */
  .noinit_ram 0x13F30 (NOLOAD) :
  {
    KEEP (*(.noinit_ram));
  } > data
}
