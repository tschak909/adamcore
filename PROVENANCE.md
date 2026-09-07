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

## Cartridge devices are out of tree

`adamcore_cart_ops` is a vtable the *host* fills in. adamcore models the
console; it ships no cartridge-mapper knowledge at all, and the flat image
loading in `cart.c` is the whole of what it knows about cartridges.

This is deliberate and is part of the clean-room position, not merely a
layering preference. The established ColecoVision mappers -- MegaCart,
Activision, X-in-1, the Opcode Super Game Cart -- are modelled in
`colmap.c`, which lives in `fujinet-firmware` (BSD-3-Clause, same author) and
whose comments state plainly that it matches MAME's handlers on purpose,
because being byte-compatible with MAME is what makes its cartridge soak test
meaningful. That is emulator-behaviour-derived knowledge. Bringing it into
this repository would make the statement above false, so it stays on the
host's side of the vtable, where its own provenance is recorded and correct.

The AY-3-8910 added for the Super Game Module was written from the GI data
sheet listed below. No emulator implementation of it -- MAME, openMSX,
blueMSX, ColEm or any other -- was opened or consulted.

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
  conventions, controller/keypad encodings, the cartridge and expansion
  connector pinouts (which is where the rule that a cartridge never sees the
  console's reset line, while an expansion module does, comes from)
- General Instrument AY-3-8910/8912 Programmable Sound Generator data sheet:
  register map and register widths, the 16-level logarithmic amplitude
  control and its 5-bit envelope resolution, the envelope shape decode
  (CONTINUE / ATTACK / ALTERNATE / HOLD), the tone, noise and envelope
  dividers, and the 17-bit noise shift register
- Published Opcode Super Game Module documentation: ports $50-$53, the $7F
  bit-1 BIOS/RAM switch, the 24K expansion at $2000-$7FFF, and the AY clock

Test material:

- Tom Harte / SingleStepTests Z80 test vectors (MIT license) — fetched by
  `tools/fetch-test-data.sh`, not vendored
- Frank Cringle's ZEXDOC/ZEXALL Z80 instruction exercisers (GPL) — fetched, not
  vendored

Author's own prior work (GPLv3, same author, reused by right):

- fujinet-pc / fujinet-firmware ADAM target (`lib/bus/adamnet`,
  `lib/device/adamnet`, `lib/hardware/BoIPChannel.cpp`) — the authoritative
  reference for the AdamNet "Bus over IP" (BoIP) wire protocol this core's
  `boip.c` implements the master side of, and for the TCP channel's
  connection/reconnection semantics
- The author's "All About AdamNet" protocol reference — packet types,
  timings, and byte-exact transaction traces
- fujinet-config sources (`src/adam`) — the guest-side expectations of the
  Fuji character-device DCB contract (e.g. status 0x8C = no data)

Interoperability analysis of shipped firmware (not emulator source):

- The Coleco EOS ROM's own AdamNet code paths were observed (via this
  core's trace tooling) and, where necessary, disassembled to pin down the
  Z80-visible master contract: PCB location and pointer variable, the
  0x80|command completion convention, EOS ownership of the DCB table, the
  DCB ADDRESS CODE field, and EOS's completion/retry status handling.
  This is behavioral analysis of the software adamcore must run — no
  emulator source was involved.

This file is updated whenever a new reference is consulted.
