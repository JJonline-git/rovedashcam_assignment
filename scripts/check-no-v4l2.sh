#!/bin/sh
# Same check the reviewers run: no V4L2 in our userspace application code.
# Exits non-zero if the app includes videodev2.h, uses VIDIOC_* ioctls or calls v4l2-ctl.
set -e
cd "$(dirname "$0")/.."
if grep -rnE 'videodev2\.h|VIDIOC_|v4l2-ctl' app/; then
    echo "FAIL: V4L2 usage found in app code" >&2
    exit 1
fi
echo "OK: no V4L2 usage in app/"
