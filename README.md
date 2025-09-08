# docker-c64
A C64 emulator that runs on a terminal window. This is a real C64 emulator "underneath", but with a
stripped down display output (just the character buffer converted to ASCII),
and no sound output. Based on https://github.com/floooh/docker-c64 but modified to have:
 - More accurate PETSCII graphics with UTF-8 character mapping
 - Scanline-accurate VIC-II timing and text line rendering
 - Autoloading and running a PRG file
 - Container image built from scratch
 - Best used with a terminal supporting xterm-256color

## Howto

Build using docker or podman:
```
> podman build . -t malafoss/c64
```

Run from Docker Hub:
```
> podman run --rm -it malafoss/c64
```

Load and run a demo.prg file:
```
> podman run --rm -it -v ./demo.prg:/demo.prg malafoss/c64 demo.prg
```

### Controls
- **PageDown**: Load file (given filename or file.prg by default)
- **PageUp**: Save file (given filename or file.prg by default)
- **End**: Toggle upper/lower case characters
- **Escape**: RUN/STOP key
- **Ctrl+C**: Exit emulator

## Other

The source code is based on these repositories:

https://github.com/floooh/docker-c64

https://github.com/floooh/chips

https://github.com/floooh/chips-test

The ready built container image:

https://hub.docker.com/r/malafoss/c64
