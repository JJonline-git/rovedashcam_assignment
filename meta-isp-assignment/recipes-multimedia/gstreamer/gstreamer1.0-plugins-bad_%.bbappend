# Only need h264parse (videoparsersbad, no external deps). The default list pulls
# librsvg -> Rust toolchain, curl, openssl, webp... hours of build for nothing we use.
PACKAGECONFIG = "${GSTREAMER_ORC}"
