# Provenance

adamcore is a clean-room implementation. It was written **without consulting the
source code of any existing ADAM/ColecoVision emulator**. In particular, the
source of AdamEm (Marcel de Kogel) and AdamEmSDL (Geoff Oltmans' SDL port),
which this core functionally replaces inside fujinet-go-adam, was never opened
during development. The compiled `adamem` binary was used only as a black-box
oracle (screen output, audio output, and loopback packet captures were compared
A/B); its source was not read.

All lookup tables in this repository (palette, flag tables, etc.) are derived
from datasheets or first principles as documented in source comments — none are
copied from another program.

## Reference materials consulted

Hardware documentation:

- Coleco ADAM Technical Reference Manual (Coleco Industries) — AdamNet PCB/DCB
  protocol, memory switcher, I/O map, device numbering
- Zilog Z80 CPU User Manual (UM008011)
- Sean Young, *The Undocumented Z80 Documented* — undocumented opcodes and flag
  behavior (X/Y flags, MEMPTR)
- David Banks (hoglet), *The Undocumented Z80 Documented* addenda and published
  research on SCF/CCF (Q register) and interrupted block-instruction flags;
  Patrik Rak's public Q-register findings
- Texas Instruments TMS9918A/TMS9928A/TMS9929A Video Display Processors
  datasheet and *Video Display Processors Programmer's Guide*
- Texas Instruments SN76489 datasheet and published ColecoVision-specific notes
  on the noise LFSR configuration
- Public ColecoVision technical documentation: I/O port map, cartridge header
  conventions, controller/keypad encodings

Test material:

- Tom Harte / SingleStepTests Z80 test vectors (MIT license) — fetched by
  `tools/fetch-test-data.sh`, not vendored
- Frank Cringle's ZEXDOC/ZEXALL Z80 instruction exercisers (GPL) — fetched, not
  vendored

Author's own prior work (GPLv3, same author, reused by right):

- fujinet-pc / fujinet-firmware ADAM target (`lib/bus/adamnet`,
  `lib/device/adamnet`) — the authoritative reference for the AdamNet
  "Bus over IP" (BoIP) wire protocol this core's `boip.c` implements the master
  side of

This file is updated whenever a new reference is consulted.
