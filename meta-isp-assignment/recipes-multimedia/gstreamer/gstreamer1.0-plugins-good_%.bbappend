# v4l2 = v4l2h264enc (Pi 4 hardware H.264 via bcm2835-codec). V4L2 is used INSIDE this
# GStreamer element, not in our application code, which is what the constraint allows.
# isomp4 (mp4mux), multifile (splitmuxsink), rtp and udp have no external deps and are always built.
PACKAGECONFIG = "${GSTREAMER_ORC} v4l2 gudev"
