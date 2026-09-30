# CH32SerialBoot / CHGame board package: notes from CHChess

CHChess is the biggest sketch on the board so far: 50.3 KB of the
50,944-byte application region, 18.0 KB of static RAM. Getting it there
changed one thing in the board package and turned up a few more worth
doing. Paths are relative to the CH32SerialBoot repository.

## The change: a *Smallest + LTO* optimisation option

**Status: in the working tree of `arduino/CHGame/boards.txt`, not committed
or released.** Package 0.2.2 does not have it.

```diff
 CHGame.menu.opt.osstd=Smallest (-Os default)
 CHGame.menu.opt.osstd.build.flags.optimize=-Os
+# Link-time optimisation: the core, the libraries and the sketch are optimised
+# as one program. The link needs no extra flag - gcc sees the LTO objects and
+# runs the plugin itself. About 3.8 KB smaller than -Os on CHChess.
+CHGame.menu.opt.oslto=Smallest + LTO (-Os -flto)
+CHGame.menu.opt.oslto.build.flags.optimize=-Os -flto
 CHGame.menu.opt.o1std=Fast (-O1)
```

What it does: `-flto` compiles every translation unit (core, libraries,
sketch) to GCC's intermediate form, and the optimisation runs once, over
the whole program, at link time. Functions are inlined across files,
constants propagate into callees (the `.constprop` clones in the map file),
and anything unreachable goes. `build.flags.optimize` is used for both
compiling and linking, so the one menu entry is enough: GCC sees the LTO
objects at link time and runs the plugin itself.

What it buys: CHChess is 50.3 KB with it and does not fit without it (the
link fails, 5.2 KB over). Without the menu entry, the same build works on
any package from the command line:

    arduino-cli compile -b CHGame:ch32v:CHGame:opt=osstd,rtlib=nano,periph=game \
        --build-property build.extra_flags=-flto CHChess

Tested with it: CHChess, release and debug builds, through all of its
development on the board. Interrupt handlers, the `osSystickHandler`
weak-alias override that CHChess's sound uses, `RAMFUNC` code in SRAM, USB
serial, and flash page writes all work under LTO. Other sketches have not
been built with it yet.

**To release it:** commit the `boards.txt` change, bump the package to
0.2.3, and regenerate the package index and archive checksums as for 0.2.2
(commits `5a4f63b`, `4b4e15e`). Then run the core's examples once with the
option selected, since LTO is the kind of change that shows up in code
nobody has built that way yet.

It could reasonably become the default for game builds. The costs are
slower links and a map file that is harder to read, since functions merge
and take `.constprop`/`.part` suffixes.

## Recommendations

In rough order of how much they would have saved on CHChess.

### 1. Report RAM against the real budget

`boards.txt` sets `upload.maximum_data_size=20480`, the whole SRAM. But the
linker script (`system/CH32X035/SRC/Ld/link_chgame_app.ld`) reserves a
fixed 2 KB stack at the top, so static data really has about 18,416 bytes.
The IDE's "Global variables use 18008 bytes (87%) ... leaving 2472 bytes
for local variables" reads as if the stack had 2.4 KB of headroom, when
the true figure is 400 bytes of static RAM left *plus* a separate 2 KB
stack. Every build also prints "Low memory available, stability problems
may occur", so the warning stops meaning anything.

Set `upload.maximum_data_size=18416` (linkable RAM minus
`__stack_size`), so the percentage is the static budget, or document the
split next to the size report.

### 2. Make the stack size selectable

`__stack_size = 2048` is fixed in the app linker script. CHChess's chess
search runs about 1.5 KB deep, and drawing a frame from inside it needs
about 800 bytes more. The game works around this by switching to a 1 KB
stack of its own for those frames (`src/Frame.cpp`, a few lines of inline
assembly). Other options:

* a Tools menu entry (2 / 3 / 4 KB) that passes
  `-Wl,--defsym=__stack_size=...` (the script already uses `PROVIDE`
  style symbols, so `--defsym` overrides cleanly), or
* documenting the side-stack pattern for sketches that call back into
  drawing from deep code.

A stack high-water helper in the core would help too (paint the stack at
boot, scan for the first overwritten word). CHChess carries its own
(`src/debug/Debug.cpp`), and it was how the 2 KB limit was found.

### 3. A save-data API in the core

Sketches can keep data in flash pages above their image; it survives
re-uploading, as the platform notes record. CHChess and CHBlackjack each
carry a copy of the flash controller sequence mirrored from
`bootloader/src/flash.c`, plus page selection, A/B pages and a CRC
(`CHChess/src/save/Save.cpp`, about 1 KB).

A small core library would remove that duplication and the risk of
getting the flash sequence subtly wrong: "give me the free pages above my
image; read, erase and write one; an A/B record with a CRC". It would also
make the linker's `APPLICATION OVERFLOW ... reaches the metadata page`
check and the free-page count visible to sketches through a symbol
instead of arithmetic.

A post-build line saying how many save pages are left would help too
(CHChess's `tools/check_size.py` prints "save pages free: 2; two need
<= 50432"). Right now you find out you lost one only if you look.

### 4. A proper RAM-function section

Flash runs with 3 wait states at 48 MHz. Measured on the board, code from
flash costs about 5 cycles an instruction against about 2 from SRAM, so
hot loops belong in RAM. Today everyone uses the same trick:

```c
#define RAMFUNC __attribute__((section(".srodata.ramfunc"), noinline))
```

It relies on the linker script placing `.srodata*` in `.data` (copied to
RAM at boot). CHGfx defines it twice and CHChess three times. A named
`.ramfunc` output section in the linker script and one `RAMFUNC(name)`
macro in the core headers would make this deliberate, visible in the map
file, and safe from a future linker-script change.

### 5. Faster memory routines

newlib-nano's `memcpy`, `memmove` and `memset` are byte loops, and they
run from flash. On a 4 bpp framebuffer that adds up: CHChess's screen
shake first used `memmove` on framebuffer rows and cost about 10 ms a
frame; a word-copy loop placed in SRAM (`fx::shiftRows`) made it cheap
enough not to show up in the frame budget. Word-aligned versions
placed in RAM (or just a fast `memcpy`/`memset` pair) in the core would
speed up every sketch that clears, scrolls or copies buffers.

### 6. Smaller things

* **The Peripherals menu** (`periph=game`, 0.2.2) is worth keeping as the
  default: about 4 KB for sketches that only need USB serial.
* **`-msave-restore`** is already in the flags: it trades a little speed
  for size (shared register save/restore routines). Worth a comment so
  nobody removes it for speed without knowing what it costs in flash.
* **Document `build.extra_flags`** as the command-line way to add flags
  (it is how CHChess builds LTO on 0.2.2).
* **The USB CDC stack is always linked** (`USBFS_IRQHandler` alone is
  about 1 KB). It is how uploads find the board, so it must stay by
  default. Still, a documented way to build without it for a
  final image might free 1-2 KB for a game that needs the last bytes, if
  the bootloader can still be entered some other way (a button held at
  reset).
