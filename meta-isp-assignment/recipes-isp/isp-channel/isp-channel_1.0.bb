SUMMARY = "Camera ISP channel: libcamera capture -> ISP -> H.264 encode, no V4L2 in app code"
LICENSE = "MIT"
LIC_FILES_CHKSUM = "file://${COMMON_LICENSE_DIR}/MIT;md5=0835ade698e0bcf8506ecda2f7b4f302"

# Source lives next to this recipe (isp-channel/app). "file://" fetches a local directory;
# BitBake checksums every file in it, so editing the code triggers a rebuild.
SRC_URI = " \
    file://app \
    file://isp-channel.service \
    file://isp-channel.default \
"
S = "${WORKDIR}/app"

# Build-time deps (headers + pkg-config files). Runtime .so deps are added automatically
# by BitBake's shlibs scan, so libcamera/gstreamer/libpng need not be listed in RDEPENDS.
DEPENDS = "libcamera gstreamer1.0 gstreamer1.0-plugins-base libpng"

inherit cmake pkgconfig systemd

SYSTEMD_SERVICE:${PN} = "isp-channel.service"
SYSTEMD_AUTO_ENABLE = "enable"

do_install:append() {
    install -D -m 0644 ${WORKDIR}/isp-channel.service ${D}${systemd_system_unitdir}/isp-channel.service
    install -D -m 0644 ${WORKDIR}/isp-channel.default ${D}${sysconfdir}/default/isp-channel
}

# GStreamer elements are loaded as plugins at runtime (not linked), so list them explicitly.
RDEPENDS:${PN} = " \
    gstreamer1.0-plugins-base-app \
    gstreamer1.0-plugins-good-video4linux2 \
    gstreamer1.0-plugins-good-isomp4 \
    gstreamer1.0-plugins-good-multifile \
    gstreamer1.0-plugins-good-rtp \
    gstreamer1.0-plugins-good-udp \
    gstreamer1.0-plugins-bad-videoparsersbad \
"
