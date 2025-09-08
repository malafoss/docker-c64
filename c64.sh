#!/bin/bash
cengine=$(which podman || which docker)
if [ $# -gt 0 ]; then
  exec $cengine run -ti --rm -v ./$1:/file.prg malafoss/c64:latest file.prg
else
  exec $cengine run -ti --rm malafoss/c64
fi
