#!/bin/sh
# mdxdump のビルド (portable_mdx を third_party から参照)
set -e
cd "$(dirname "$0")"
PMDX=../../third_party/portable_mdx
mkdir -p objs
for f in mxdrv mxdrv_context sound_iocs; do
  c++ -O2 -c -std=c++11 -I$PMDX/include $PMDX/src/mxdrv/$f.cpp -o objs/$f.o
done
for f in x68sound_adpcm x68sound_lfo x68sound_op x68sound_opm x68sound_pcm8 x68sound x68sound_context; do
  c++ -O2 -c -std=c++11 -I$PMDX/include $PMDX/src/x68sound/$f.cpp -o objs/$f.o
done
cc -O2 -c -I$PMDX/include $PMDX/src/mdx_util.c -o objs/mdx_util.o
cc -O2 -c -I$PMDX/include mdxdump.c -o objs/mdxdump.o
c++ objs/*.o -o mdxdump
echo "built: $(pwd)/mdxdump"
