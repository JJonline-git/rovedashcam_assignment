#!/bin/sh
# Same check the reviewers run: no V4L2 in our userspace application code.
# Fails if the app includes videodev2.h, uses VIDIOC_* ioctls or calls v4l2-ctl.
# grep exit codes: 0 = match (FAIL), 1 = no match (OK), 2 = grep error (FAIL, never a silent pass).
cd "$(dirname "$0")/.." || exit 2
SRC=meta-isp-assignment/recipes-isp/isp-channel/isp-channel/app
[ -d "$SRC/src" ] || { echo "FAIL: $SRC/src not found" >&2; exit 2; }
grep -rnE 'videodev2\.h|VIDIOC_|v4l2-ctl' "$SRC"
case $? in
    0) echo "FAIL: V4L2 usage found in app code" >&2; exit 1 ;;
    1) echo "OK: no V4L2 usage in $SRC" ;;
    *) echo "FAIL: grep error" >&2; exit 2 ;;
esac
