#!/bin/bash
set -e

export LD_LIBRARY_PATH=/usr/local/src/libav_for_drawtext
AVCONV=/usr/local/src/libav_for_drawtext/avconv
APP=/usr/local/src/interval_transcoding/interval_transcoding

FILTERCHAIN="drawtext=text='$TEXT'"\
":fontfile='$FONTFILE':fontcolor=$COLOR"\
":x=$X:y=$Y:fontsize=$FONTSIZE"\
""

TMPDIR=`mktemp --tmpdir --directory drawtext.XXXXXXX`
function cleanup {
    rm -rf $TMPDIR
    exit 0
}
trap cleanup INT TERM QUIT
pushd $TMPDIR &>/dev/null

$AVCONV -i $INFILE -c copy -bsf h264_mp4toannexb annexb.ts
$APP -i annexb.ts -o out.ts --filterchain "$FILTERCHAIN" \
    --filter_start $FILTER_START --filter_end $FILTER_END
$AVCONV -i out.ts -c copy out.mp4
cat out.mp4
cleanup
