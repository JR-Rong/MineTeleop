#!/usr/bin/env bash
set -euo pipefail
# HCK-C1202 ISX031 native mode. The media-bus code is intentionally YUYV8_1X16;
# the V4L2 video node delivers UYVY. See docs/orin-isx031-cpu.md.
if [[ $# != 4 ]]; then
  echo "Usage: $0 /dev/videoN /dev/mediaN SERIALIZER_ENTITY DESERIALIZER_ENTITY" >&2
  echo "Inspect media-ctl -p first; entity names depend on camera wiring." >&2
  exit 2
fi
video=$1
media=$2
serializer=$3
deserializer=$4
[[ $video =~ ^/dev/video[0-9]+$ && $media =~ ^/dev/media[0-9]+$ &&
   $serializer =~ ^ser_[0-9]+_ch_[0-9]+$ && $deserializer =~ ^des_[0-9]+_ch_[0-9]+$ ]] || {
  echo 'Invalid device path or SerDes entity name' >&2; exit 2;
}
for tool in media-ctl v4l2-ctl fuser; do command -v "$tool" >/dev/null; done
[[ -c $video && -c $media ]] || { echo 'Video/media device is missing' >&2; exit 1; }
if fuser "$video" >/dev/null 2>&1; then
  echo "Stop capture on $video before changing its media format" >&2; exit 1
fi
# Resolve both entities before making any change; media-ctl rejects missing names.
media-ctl -d "$media" --entity "$serializer" >/dev/null
media-ctl -d "$media" --entity "$deserializer" >/dev/null
v4l2-ctl -d "$video" --get-ctrl=sensor_mode,bypass_mode >/dev/null
media-ctl -d "$media" --set-v4l2 "\"$serializer\":1[fmt:YUYV8_1X16/1920x1536]"
media-ctl -d "$media" --set-v4l2 "\"$deserializer\":0[fmt:YUYV8_1X16/1920x1536]"
v4l2-ctl -d "$video" --set-ctrl=sensor_mode=0,bypass_mode=0 \
  --set-fmt-video=width=1920,height=1536,pixelformat=UYVY --get-fmt-video
