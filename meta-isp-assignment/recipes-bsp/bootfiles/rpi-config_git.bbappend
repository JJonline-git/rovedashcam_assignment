# config.txt settings for the camera. rpi-config generates /boot/config.txt from these.

# gpu_mem: with the KMS driver the GPU (VideoCore) no longer holds camera/display buffers;
# those come from Linux CMA. But the hardware H.264 encoder (bcm2835-codec) still runs
# as VideoCore firmware and needs some GPU-side memory. 128 MB is the value Raspberry Pi
# documents for camera + H.264 use; the 76 MB default is tight for 1080p encode.
GPU_MEM = "128"

# CMA (Contiguous Memory Allocator): every camera/ISP/encoder buffer is a DMA buffer
# allocated from CMA. Rough budget for our pipeline:
#   viewfinder 1920x1080 YUV420  = 3.1 MB x 6 buffers  ~ 19 MB
#   raw 2592x1944 10-bit packed   = 6.3 MB x 6 buffers  ~ 38 MB
#   ISP internals (stats, LS tables) + encoder in/out buffers ~ 30 MB
#   total ~ 90 MB  -> 256 MB gives ~2.5x headroom (and room for a display later).
VC4DTBO = "vc4-kms-v3d,cma-256"

# Serial console on GPIO14/15 (115200) so we can capture the boot log.
ENABLE_UART = "1"

# Do NOT set VIDEO_CAMERA=1: that adds start_x=1, the legacy firmware camera stack,
# which conflicts with libcamera. We bind the sensor explicitly instead of
# camera_auto_detect, so boot does not depend on auto-probing a 3rd-party module.
RPI_EXTRA_CONFIG = "\n# --- ISP assignment: camera ---\ncamera_auto_detect=0\ndtoverlay=ov5647\n"
