# Classic Gameboy implementation

## Build

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build build -j
```

SDL3 is optional. If it is missing, the build still produces a headless host:

```sh
brew install sdl3                 # macOS
sudo apt install libsdl3-dev       # Debian/Ubuntu
cmake -S . -B build -DGBEMU_ENABLE_SDL=OFF     # force headless
```

## Run

```sh
build/gbemu roms/homebrew.gb                      # play in a window
build/gbemu --headless --frames 600 rom.gb          # run headless, print serial
build/gbemu --headless --frames 200 --frame-hash rom.gb
build/gbemu --headless --frames 4 --dump-frame out.bmp rom.gb
build/gbemu --headless --frames 90 --press 60:A --serial-log out.txt rom.gb
build/gbemu --cgb --headless --frames 600 colour.gb
build/gbemu --boot-rom dmg_boot.bin rom.gb        # run a boot ROM first
build/gbemu --trace --break 40 rom.gb               # trace, break on VBlank
build/gbemu --debug rom.gb                          # interactive debugger
```

`build/gbemu --help` lists every option. Headless mode never creates a window
and never opens an audio device, which is what lets CI grade the emulator.

## Tests

```sh
ctest --test-dir build --output-on-failure
```

Two test suites run:

- `core` — `tests/test_flags.c`: registers, flags, cycle counts, interrupts,
  the HALT bug, timers, the joypad matrix, open bus, serial, OAM DMA and a
  save-state round trip. No ROM needed.
- `emulator` — `tests/run_tests.sh`: generates the course's own test ROMs with
  `tools/make_test_roms.py`, runs the emulator headless, and asserts on the
  strings the ROMs print. The PPU check goes further: `tests/check_frame.py`
  rebuilds the expected image from the ROM's own tile and map definitions and
  compares it with the dumped frame.


## Save states

`gb_save_state`/`gb_load_state` write a versioned, fixed-order dump of every
subsystem. The format is **same build, same machine only**: it stores native struct
layout, so a state file is not portable across compilers, architectures or versions.

## Legal

No commercial ROMs and no Nintendo boot ROM are included, and none should ever be
committed. The test ROMs in `tools/make_test_roms.py` are generated from scratch
and are free to use.
