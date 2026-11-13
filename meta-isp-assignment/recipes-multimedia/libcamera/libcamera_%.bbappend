# Build only the pipeline handler we use. On a Pi 4 that is "rpi/vc4"
# (Unicam CSI-2 receiver + bcm2835-isp hardware ISP, with the rpi/vc4 IPA for AE/AWB).
# meta-raspberrypi already adds "-Dpipelines=rpi/vc4" through PACKAGECONFIG; setting
# LIBCAMERA_PIPELINES too keeps the two in agreement instead of "auto" + override.
LIBCAMERA_PIPELINES:rpi = "rpi/vc4"
