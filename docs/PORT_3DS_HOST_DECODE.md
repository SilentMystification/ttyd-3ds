# Host decode tool

The 3DS port converts the game's data at load time from the user's disc image: TPL textures, animated
models, maps, camera roads, fonts, `icon.bin` and the MusyX files are big-endian on the disc and are
byte-swapped in place as they are read (`port3ds/game/formats/`, `port3ds/game/port_swap.c`,
`port3ds/game/musyx_swap.c`, from the tables `tools/port3ds/gen_swap.py` generates). The host decode tool
runs **those same sources** on the machine you develop on, so "every file on the disc decodes" can be
checked without a console or an emulator (PORT_3DS_PLAN.md section 10, phase 2).

## What it is

`tools/port3ds/host_decode/` is a separate CMake project (the host compiler, no devkitPro) that compiles:

| Compiled as they are in the port | Written for the host |
| --- | --- |
| `port3ds/game/formats/{tpl,anim,map,font,camroad}.c` | `main.c`: the command line, the reports |
| `port3ds/game/port_swap.c`, `port3ds/game/musyx_swap.c` | `fst.c`: the disc header and file table |
| `port3ds/game/disc_data.c` (the data read from the image) | `shim.c`: logging, `plat_fatal`/`OSPanic`, disc reads |
| the generated swap tables (`gen_swap.py`) | `host_prefix.h`: the decomp's integer types, fixed width |
| | `formats_ttyd.c`: which disc files get which converter |

The shim is the only platform surface the converters touch: `plat_log`, `plat_fatal` and `OSPanic` (here a
`longjmp` back to the caller, which reports the file and the message), `calloc`/`free`, and
`plat_disc_read`. Nothing in `main.c`, `fst.c` or `shim.c` knows a game's formats: another game supplies its
own `formats_<game>.c` (`-DHOST_DECODE_FORMATS=...`) and the same tool runs its converters (PORT_3DS_PLAN.md
section 1, portability).

Two details make the host build faithful:

* The swap tables come from the DWARF of a 32-bit probe object (`gcc -m32 -malign-double`, compiled with
  `-ffreestanding` so no 32-bit C library is needed). On the 3DS the tables come from the ARM probe. The two
  were compared: `gen_swap.py tables --host` on the real ARM probe object and on the host probe object give
  byte-identical output.
* `host_prefix.h` defines `u32` as a 32-bit integer (the decomp's `dolphin/types.h` defines it as `unsigned
  long`, which is 64 bits on a 64-bit host) and keeps that header out.

## Build and usage

```sh
cmake -S tools/port3ds/host_decode -B build/host_decode -G Ninja && cmake --build build/host_decode
export TTYD_DISC=iso/game.iso          # or --disc IMAGE; read only, never modified
build/host_decode/host_decode list [-l]                      # the FST (-l: offsets, sizes, converters)
build/host_decode/host_decode decode PATH [-o OUT] [--format NAME]
build/host_decode/host_decode all [--out DIR] [--tsv FILE] [--only FORMAT] [-q]
build/host_decode/host_decode discdata                       # the arrays the port reads from main.dol and the RELs
build/host_decode/host_decode selftest                       # swap tables applied twice are the identity
```

`decode` reads the file as the game does (rounded up to 32 bytes where the game does, zero padded), runs the
converter, and writes the converted bytes (default `decoded/<path>`). `all` does that for every file a
converter row matches and reports, per format: files, failures, files left unchanged, bytes, converter time
and disc read time. Each converter that is meant to be idempotent (TPL, anim, map, camroad, font: the game
may open a file twice) is run a second time on its own output, which must change nothing. `-v`/`-vv` show
the converters' own warnings and debug lines.

Formats (`formats_ttyd.c`): `tpl` and `tpl-nosize` (every TPL, converted with its size as filemgr does and
with `PORT_SWAP_SIZE_UNKNOWN` as `UnpackTexPalette` does), `anim` (`a/<name>`), `map` (`m/*/d`), `camroad`
(`m/*/c`), `font` (`*.bfn`), `icon.bin`, `musyx-proj|pool|sdir|slib` (`sound/`), `musyx-stm` (a stream's
header). Files with no converter (messages, RELs, `.samp`, `m/*/s`, ...) are used by the game as they are.

## Tests

```sh
tools/port3ds/test_decode.py [--disc iso/game.iso] [--papercut ../paper-cut] [--full] [--keep DIR]
```

Builds the tool and runs everything below, printing one line per check and a summary (exit 1 on a failure).
paper-cut (MIT, `../paper-cut`, found next to the repository or by `--papercut`/`$PAPERCUT`) is read only and
is never built in place; without it the cross-checks are skipped. Run time is about one to three minutes.

| Check | What is compared |
| --- | --- |
| swap tables twice | every generated struct table on random data: one pass reverses exactly the bytes of the runs, two passes are the identity; `port_swap_units` at widths 2, 4, 8; `portSwapU16Run`/`U32Run` |
| FST | host_decode's listing (path, offset, size of every file) against paper-cut's `tools/port/gcdisc.py` |
| `all` | every file converts, converting again changes nothing, no converter returned early (every converted file differs from the disc file) |
| `discdata` | the arrays `port_disc_data_fill` reads (memory card art, `texture_0`, torch and `win` textures) against an independent read of `main.dol` sections and the REL section tables |
| TPL | every texture header (size, format, data and palette offsets, wrap, filters) of the converted header graph against paper-cut's `ttyd_tpl.py` parse of the disc file; decoded pixels (every format, palettes included) through paper-cut's `tpl2png.py`, for a sample (every 25th TPL; `--full`: all) |
| anim | header counts, bounds, names, group tree, node floats, visibility bytes, vertex position, normal and texture-coordinate floats, index tables, animation table names, loop info, keyframe count and times, value keys, against paper-cut's `ttyd_anim.py` parse |
| map | header, chunk names, version, joint tree (names, types, bounds, scale/rotation/translation), mesh headers and display list sizes, vertex array counts and per-axis min/max (f32 or s16), light, animation and texture tables: a walk of the converted file in little-endian against the same walk of the disc file in big-endian, with the layout of paper-cut's `mapdump.py` |
| permutation | the bytes of every converted file are a permutation of the disc file's (a converter only reverses bytes inside 2/4/8-byte units; the camera-road marker excepted) |
| MusyX | `.proj`, `.pool`, `.slib` and `.stm` headers byte-identical to paper-cut's `musyx_swap.c` (compiled as it is, `host_decode/xcheck_musyx.c`); `.sdir` entries (id, reference count, offset, address, sample header, ADPCM offset) and the ADPCM records against paper-cut's rebuilt native directory |
| paper-cut C decoders | paper-cut's `tests/port/disc_sweep.c` (its `portTplDecode`, `portAnimDecode`, `portMapDecode`, `portCamRoadDecode`, `portFontSwap`, compiled with gcc) over the same files: both accept the same set |

Not covered, and why: the FST reader is a copy of the logic in `port3ds/game/dvd.c` (`DVDInit`,
`DVDConvertPathToEntrynum`), not that file (it is built on the 3DS thread and interrupt layer); texel and CLUT
data, packed display lists and vertex colours are left big-endian by the converters (the GX layer reads them
so), so they are compared only as unchanged bytes; camera roads have no paper-cut Python decoder, so they are
covered by the permutation check and paper-cut's C decoder accepting them.

## Results on the US disc (G8ME01)

`host_decode all` over the 3,204 files: 0 failures, about 2.4 s of conversion and 0.7 to 1.2 s wall time with
the image in the page cache (the tables below are the converter time).

| Format | Files | Failures | Bytes | Convert |
| --- | --- | --- | --- | --- |
| tpl (with size) | 1,263 | 0 | 131.0 MB | 7 ms |
| tpl (no size) | 1,263 | 0 | 131.0 MB | 4 ms |
| anim | 656 | 0 | 35.6 MB | 70 ms |
| map | 333 | 0 | 152.1 MB | 340 ms |
| camroad | 280 | 0 | 0.3 MB | 1.5 ms |
| font | 3 | 0 | 0.4 MB | < 1 ms |
| icon.bin | 1 | 0 | 8 KB | < 1 ms |
| musyx proj, pool, sdir, slib | 1 each | 0 | 0.5 MB | 2 ms |
| musyx stm (header) | 146 | 0 | 259.5 MB on disc | < 1 ms |

518 files (21.7 MB) have no converter: messages, RELs, `.samp`, `.db`, `m/*/s` and the like.

One file differs from paper-cut: `a/OFF_hei_01b` is a pre-release model (0x68-byte shapes) that paper-cut's
decoder rejects. The port's converter accepts it and swaps it with the retail layout, which is wrong for it;
the game never loads it (`hei_01` loads `OFF_hei_01c`). `test_decode.py` lists it as a known difference.
