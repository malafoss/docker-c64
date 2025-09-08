#!/bin/bash
cengine=$(which podman || which docker)

# Parse arguments to separate options from filename
options=""
filename=""

while [[ $# -gt 0 ]]; do
  case $1 in
    -*)
      options="$options $1"
      shift
      ;;
    *)
      filename="$1"
      break
      ;;
  esac
done

if [ -n "$filename" ]; then
  exec $cengine run -ti --rm -v "./$filename:/$filename.prg" malafoss/c64:latest $options $filename.prg
else
  exec $cengine run -ti --rm malafoss/c64:latest $options
fi
