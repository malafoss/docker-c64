# c64

A C64 emulator that runs in a terminal window. This is a real C64 emulator underneath, with
pixel-accurate graphics output via the sixel or kitty terminal graphics protocols, or a
Unicode PETSCII text mode fallback. No sound output. Based on
https://github.com/floooh/docker-c64 but modified to have:

- Pixel graphics output via sixel or kitty protocols (auto-detected)
- Full PAL TV crop with accurate border rendering
- More accurate PETSCII text mode with UTF-8 character mapping
- Scanline-accurate VIC-II timing
- Autoloading and running a PRG file
- Container image built from scratch

## Quick start

The easiest way to run is via the included `c64.sh` wrapper script, which handles mounting
PRG files and passes arguments through to the container:

```
./c64.sh
./c64.sh demo.prg
./c64.sh --mode=sixel demo.prg
```

`c64.sh` uses `podman` if available, otherwise `docker`.

## Manual usage

Build:
```
podman build . -t malafoss/c64
```

Run from Docker Hub:
```
podman run --rm -it malafoss/c64
```

Load and run a PRG file:
```
podman run --rm -it -v ./demo.prg:/demo.prg malafoss/c64 /demo.prg
```

## Output modes

The `--mode=` option selects the graphics output. The default is `auto`, which probes
the terminal at startup and picks the best available mode.

| Mode     | Description                                         |
|----------|-----------------------------------------------------|
| `auto`   | Auto-detect kitty or sixel, fall back to text (default) |
| `kitty`  | Kitty terminal graphics protocol                    |
| `sixel`  | Sixel graphics protocol (xterm, mlterm, foot, …)   |
| `narrow` | Unicode PETSCII text, narrow characters (1:1 ratio) |
| `wide`   | Unicode PETSCII text, wide characters (2:1 ratio)   |

Examples:
```
podman run --rm -it malafoss/c64 --mode=kitty
podman run --rm -it malafoss/c64 --mode=sixel
podman run --rm -it malafoss/c64 --mode=narrow
```

Or via the wrapper:
```
./c64.sh --mode=kitty demo.prg
```

## Controls

| Key          | Action                                      |
|--------------|---------------------------------------------|
| PageDown     | Load file (given filename or `file.prg`)    |
| PageUp       | Save file (given filename or `file.prg`)    |
| End          | Toggle upper/lower case characters          |
| Escape       | RUN/STOP key                                |
| Ctrl+C       | Exit emulator                               |

## Source

See https://github.com/malafoss/docker-c64

Based on:

- https://github.com/floooh/docker-c64
- https://github.com/floooh/chips
- https://github.com/floooh/chips-test

Pre-built container image: https://hub.docker.com/r/malafoss/c64
