SUMMARY = "Minimal image that runs the camera ISP channel at boot"
LICENSE = "MIT"

# Start from core-image-minimal (busybox + packagegroup-core-boot) and add only what we need.
require recipes-core/images/core-image-minimal.bb

# SSH (dropbear is small). Empty root password is for a lab/dev image only;
# a product image would use keys and drop these three features.
IMAGE_FEATURES += "ssh-server-dropbear allow-empty-password empty-root-password allow-root-login"

IMAGE_INSTALL:append = " \
    isp-channel \
    libcamera \
    kernel-modules \
    systemd-conf \
"
# isp-channel  : our app + systemd unit (pulls GStreamer plugins via RDEPENDS)
# libcamera    : also ships the "cam" tool, handy for debugging the sensor
# kernel-modules: ov5647, bcm2835-unicam, bcm2835-isp, bcm2835-codec, vc-sm-cma are modules
# systemd-conf : default wired.network -> DHCP on eth0 via systemd-networkd (minimal networking)

# 1 GiB of free space on the rootfs for recorded video segments.
IMAGE_ROOTFS_EXTRA_SPACE = "1048576"
