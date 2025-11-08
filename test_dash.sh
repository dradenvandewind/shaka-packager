if [ -z "${1}" ]; then
    echo "Error: no file in input"
    exit 1
fi

packager \
  "in=${1},stream=video,init_segment=init.mp4,segment_template=seg_\$Number\$.m4s" \
  --mpd_output /home/manifest.mpd \
  --v 3
