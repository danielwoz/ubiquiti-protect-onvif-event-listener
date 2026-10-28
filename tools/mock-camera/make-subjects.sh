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

fit="scale=1280:720:force_original_aspect_ratio=decrease,format=rgba,\
pad=1280:720:(ow-iw)/2:(oh-ih)/2:color=black@0.0"
ff -i "$src/person.jpg" -vf "$fit" -frames:v 1 "$out/person.png"
ff -i "$src/animal.jpg" -vf "$fit" -frames:v 1 "$out/animal.png"
# The right-hand part of the 1050x656 vehicle image: the parked SUV, without the caption
# and the annotation circle on the left.
ff -i "$src/vehicle.jpg" -vf "crop=560:656:490:0,$fit" -frames:v 1 "$out/vehicle.png"

# A cardboard box with tape and a label on a doorstep, in front of a door. Drawn opaque
# (drawbox keeps the canvas alpha, so it cannot draw onto the transparent blank) and
# letterboxed like the photos.
ff -f lavfi -i "color=c=0xB9B4AA:s=960x720,\
drawbox=x=300:y=40:w=360:h=500:color=0x5C4033@1:t=fill,\
drawbox=x=0:y=520:w=960:h=200:color=0x7A7A7A@1:t=fill,\
drawbox=x=310:y=330:w=340:h=260:color=0xA0703C@1:t=fill,\
drawbox=x=310:y=290:w=340:h=40:color=0xC08A50@1:t=fill,\
drawbox=x=460:y=290:w=40:h=300:color=0xD9C9A3@1:t=fill,\
drawbox=x=340:y=470:w=110:h=70:color=0xF2F2F2@1:t=fill,\
drawbox=x=310:y=290:w=340:h=300:color=0x5A3A1A@1:t=4" -vf "$fit" -frames:v 1 \
  "$out/package.png"

ls -l "$out"
