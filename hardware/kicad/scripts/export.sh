#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later
# Regenerate every output under hardware/ from the KiCad files.
# Run from the repository root:  sh hardware/kicad/scripts/export.sh
set -e
K="flatpak run --env=KICAD10_3DMODEL_DIR=/app/extensions/Library/3dmodels --command=kicad-cli org.kicad.KiCad"
cd hardware/kicad
P=bc250-power-controller
OUT=..

# schematic
$K sch export pdf --output $OUT/schematic.pdf $P.kicad_sch
mkdir -p ../../.export-tmp
$K sch export svg --no-background-color --output ../../.export-tmp $P.kicad_sch
cp ../../.export-tmp/$P.svg $OUT/schematic.svg

# renders
mkdir -p $OUT/renders
$K pcb render --output ../../.export-tmp/top.png --side top --width 1600 --height 1100 --quality basic --zoom 1.25 $P.kicad_pcb
$K pcb render --output ../../.export-tmp/bottom.png --side bottom --width 1600 --height 1100 --quality basic --zoom 1.25 $P.kicad_pcb
$K pcb render --output ../../.export-tmp/3d.png --width 1600 --height 1100 --quality basic --perspective --rotate "-40,0,25" --zoom 0.8 $P.kicad_pcb
for n in top bottom 3d; do
  magick ../../.export-tmp/$n.png -strip -colors 192 PNG8:$OUT/renders/pcb-$n.png
done

# fabrication files
rm -rf ../../.export-tmp/fab && mkdir -p ../../.export-tmp/fab
$K pcb export gerbers --output ../../.export-tmp/fab/ --layers F.Cu,B.Cu,F.Silkscreen,B.Silkscreen,F.Mask,B.Mask,Edge.Cuts --subtract-soldermask $P.kicad_pcb
$K pcb export drill --output ../../.export-tmp/fab/ --format excellon --drill-origin absolute --excellon-units mm --excellon-separate-th $P.kicad_pcb
mkdir -p $OUT/fab
rm -f $OUT/fab/$P-gerbers.zip
ROOT=$(cd ../..; pwd); (cd ../../.export-tmp/fab && zip -q -j $ROOT/hardware/fab/$P-gerbers.zip *)
rm -rf ../../.export-tmp
