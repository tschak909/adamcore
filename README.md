# adamcore

A clean-room, from-scratch emulator core for the Coleco ADAM and ColecoVision,
written in portable C99 and licensed under the GNU GPL v3.

adamcore is a library, not an application. It has no rendering, audio, or input
code of its own: the host embeds it, drives `adamcore_run_frame()`, blits the
RGB565 framebuffer, pulls audio samples, and injects input. The primary host is
the [fujinet-go-adam](https://github.com/FujiNetWIFI/fujinet-go-adam) Android
app; a small Linux CLI runner lives in `cli/` for development and testing.

## What it emulates

- Zilog Z80 CPU (documented and undocumented behavior, MEMPTR, Q register),
  validated against Tom Harte's SingleStepTests and ZEXDOC/ZEXALL
- TI TMS9928A VDP (modes 0–3, sprites, 256×212 output incl. borders)
- TI SN76489 PSG (pull-model resampling to the host audio rate)
- Coleco ADAM memory switcher and ColecoVision I/O map
- ColecoVision cartridges
- A high-level AdamNet master: local keyboard device, plus "Bus over IP" (BoIP)
  forwarding of storage/network devices over loopback TCP to a connected
  [FujiNet](https://fujinet.online/) (fujinet-pc built for the ADAM target)

Out of scope (v1): local disk/tape images, printer, IDE, snapshots, SGM.
Storage is expected to be served by FujiNet over BoIP.

## Provenance

This core was written without reference to any existing emulator's source code.
See [PROVENANCE.md](PROVENANCE.md) for the clean-room statement and the complete
list of reference documents.

No ROM images are included in or distributed with this repository.

## Building

```
cmake -B build && cmake --build build      # or: make
tools/fetch-test-data.sh                   # test vectors (needs network)
tools/run-all-tests.sh
```

## License

GPL-3.0-or-later. Copyright (C) 2026 Thomas Cherryhomes.
