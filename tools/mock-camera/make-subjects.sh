#!/bin/sh
# Renders the scenario subject overlays: 1280x720 RGBA PNGs with the photo centred on a
# transparent canvas, plus a fully transparent "none" and a drawn "package".
#
# usage: make-subjects.sh <dir with person.jpg vehicle.jpg animal.jpg> <output dir>
set -eu
src=$1
out=$2
mkdir -p "$out"

ff() { ffmpeg -hide_banner -loglevel error -y "$@"; }

blank="color=c=black@0.0:s=1280x720,format=rgba"
ff -f lavfi -i "$blank" -frames:v 1 "$out/none.png"

for name in person vehicle animal; do
  ff -i "$src/$name.jpg" -vf "scale=1280:720:force_original_aspect_ratio=decrease,format=rgba,\
pad=1280:720:(ow-iw)/2:(oh-ih)/2:color=black@0.0" -frames:v 1 "$out/$name.png"
done

# A cardboard box with tape and a label, standing on a grey doorstep.
ff -f lavfi -i "$blank,\
drawbox=x=0:y=560:w=1280:h=160:color=0x6E6E6E@1:t=fill,\
drawbox=x=470:y=330:w=340:h=260:color=0xA0703C@1:t=fill,\
drawbox=x=470:y=290:w=340:h=40:color=0xB8844A@1:t=fill,\
drawbox=x=620:y=290:w=40:h=300:color=0xD9C9A3@1:t=fill,\
drawbox=x=500:y=470:w=110:h=70:color=0xF2F2F2@1:t=fill,\
drawbox=x=470:y=290:w=340:h=300:color=0x5A3A1A@1:t=4" -frames:v 1 "$out/package.png"

ls -l "$out"
