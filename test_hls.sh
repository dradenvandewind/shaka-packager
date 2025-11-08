if [ -z "${1}" ]; then
    echo "Error: no file in input"
    exit 1
fi

packager \
  "in=${1},stream=video,playlist_name=video.m3u8,segment_template=seg_\$Number\$.m4s" \
  --hls_master_playlist_output /home/master.m3u8 \
  --v 3
