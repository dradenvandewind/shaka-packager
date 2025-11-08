if [ -z "${1}" ]; then
    echo "Error: no file in imput"
    exit 1
fi
packager "in={1},stream=video,output=/home/output.mp4" --v 3
