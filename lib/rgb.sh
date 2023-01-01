#!/bin/bash

RGBRAW="rgb.txt"
RGBURL="https://gitlab.freedesktop.org/xorg/app/rgb/raw/master/$RGBRAW"
RGBHEX="rgb.inc"
RGBHTM="../website/rgb.htm"
RGBHTML="../website/rgb.html"

usage() {
    [[ -n $@ ]] && echo -e "Error: $@\n"
    echo "Usage: ${0##*/} [-d] [-g]"
    echo "  -d download a fresh copy of $RGBRAW"
    echo "  -g generate $RGBHEX from $RGBRAW"
}

generate_inc() {
    [[ -f $RGBHEX ]] && rm $RGBHEX
    while read -r line; do
        if (echo "$line" | cut -d' ' -f4- | grep -v ' ' >/dev/null); then
            printf "{ \"%s\", \"0x" $(echo "$line" | cut -d' ' -f4) >>$RGBHEX
            printf "%02X"           $(echo "$line" | cut -d' ' -f1) >>$RGBHEX
            printf "%02X"           $(echo "$line" | cut -d' ' -f2) >>$RGBHEX
            printf "%02X\" },\n"    $(echo "$line" | cut -d' ' -f3) >>$RGBHEX
        fi
    done < <(cat $RGBRAW | sed -E 's/^[[:blank:]]*|[[:blank:]]*$//g; s/[[:blank:]][[:blank:]]*/ /g')
}

[[ $# -gt 0 ]] || usage

while getopts ":dg" o; do
    case $o in
        d)  curl -o $RGBRAW $RGBURL
            ;;
        g)  generate_inc
            ;;
        \?) usage "Invalid parameter \"-$OPTARG\""
            ;;
    esac
done
