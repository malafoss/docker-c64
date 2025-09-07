# docker-c64
A C64 emulator in Docker. This is a real C64 emulator "underneath", but with a
stripped down display output (just the character buffer converted to PETSCII graphics),
and no sound output. Based on https://github.com/floooh/docker-c64 but modified to have:
 - Accurate PETSCII graphics with UTF-8 character mapping
 - Scanline-accurate VIC-II timing and rendering
 - Automatic file loading and execution
 - Real-time per-line screen updates
 - Container image built from scratch

## Usage

### Command Line Options
```
./c64 [OPTIONS] [filename]

Options:
  -h, --help    Show help message

Arguments:
  filename      PRG file to auto-load and run (default: file.prg for manual loading)
```

### Examples
```bash
# Run with default file.prg (manual loading with PageDown)
./c64

# Auto-load and run a specific file
./c64 demo.prg

# Show help
./c64 --help
```

### Controls
- **PageDown**: Load file
- **PageUp**: Save file  
- **End**: Toggle upper/lower case characters
- **Escape**: RUN/STOP key
- **Ctrl+C**: Exit emulator

## Docker Usage

Build using docker or podman:
```
> podman build . -t c64
```

Run from Docker Hub:
```
> podman run --rm -it malafoss/c64
```

Auto-load and run a local PRG file:
```
> podman run --rm -it -v ./demo.prg:/demo.prg malafoss/c64 demo.prg
```

Run with default file.prg (manual loading):
```
> podman run --rm -it -v ./myprogram.prg:/file.prg malafoss/c64
```

The source code is based on these repositories:

https://github.com/floooh/docker-c64

https://github.com/floooh/chips

https://github.com/floooh/chips-test
