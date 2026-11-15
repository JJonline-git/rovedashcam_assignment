# ROVE take-home, Track H: camera ISP channel on Raspberry Pi 4

A camera ISP channel on a **Raspberry Pi 4 + OV5647**, built as a Yocto image with kas.

The camera feeds the Pi's hardware ISP, which feeds the hardware H.264 encoder, which writes MP4 files in a loop. The app uses **only the libcamera and GStreamer APIs**; it contains no V4L2 calls.

```
OV5647 → CSI-2 → Unicam → ISP (bcm2835-isp) → libcamera → isp-channel → H.264 encoder → MP4 segments
                                                              │  (dmabuf, zero-copy)  └→ RTP/UDP (optional)
                                                              └→ snapshot: PNG + raw Bayer
```

- **Track:** H (Raspberry Pi, Yocto). The sensor is a Sunny P5V04A (OV5647, same as Camera Module v1). The recruiter confirmed it as a substitute for the IMX219.
- **Yocto:** Scarthgap 5.0 LTS, built with kas, every layer pinned to a commit.
- **Measured on hardware** (10-minute run, `--raw video`):
  - 1080p30 YUV420 viewfinder plus a raw Bayer stream, zero-copy into the hardware H.264 encoder;
  - **17,995 frames, 0 drops, 30.00 fps, 14 % CPU (34 % without zero-copy), +256 kB RSS.**
- **Service default (`--raw full`):** raw at the full 2592x1944 sensor mode, as the brief asks. The OV5647 runs that mode at **15.6 fps at most**, so the viewfinder also runs at 15.6 fps. See [Known limitations](#7-known-limitations).

The design, measurements, trade-offs and debugging notes are in **[REPORT.md](REPORT.md)**.
The image and the screen recording are in the **[v1.0 release](https://github.com/JJonline-git/rovedashcam_assignment/releases/tag/v1.0)**.

---

## What you need

- Raspberry Pi 4 Model B, with a 5 V / 3 A USB-C power supply
- an OV5647 camera module (Camera Module v1 or compatible) and its 15-pin ribbon cable
- a microSD card of **at least 4 GB**
- Ethernet to a network with DHCP (the image has no Wi-Fi setup)
- a Linux PC for flashing; optional: a 3.3 V USB-serial adapter for the console

## 1. Quick start: flash the prebuilt image

```sh
# download from the v1.0 release
wget https://github.com/JJonline-git/rovedashcam_assignment/releases/download/v1.0/isp-image-raspberrypi4-64.rootfs-20261114101812.wic.bz2
wget https://github.com/JJonline-git/rovedashcam_assignment/releases/download/v1.0/isp-image-raspberrypi4-64.rootfs-20261114101812.wic.bmap
sha256sum isp-image-raspberrypi4-64.rootfs-20261114101812.wic.bz2
# expected: 77dd9edeebf191060d9e353cf80699234a40fc4eb78a244e0b866bcb9b062c2f

lsblk                  # find the SD card, e.g. /dev/sdX. Check it twice: this erases the disk.
sudo umount /dev/sdX?* 2>/dev/null
sudo bmaptool copy isp-image-raspberrypi4-64.rootfs-20261114101812.wic.bz2 /dev/sdX
# without bmaptool: bzcat isp-image-...wic.bz2 | sudo dd of=/dev/sdX bs=4M conv=fsync
```

Then continue with [3. Boot and connect](#3-boot-and-connect).

## 2. Build the image yourself

**Host:** Ubuntu 22.04 with the Yocto host packages ([Yocto docs: Build Host Packages](https://docs.yoctoproject.org/5.0/ref-manual/system-requirements.html#ubuntu-and-debian)) plus kas. The finished build folder is about 18 GB (with `rm_work`); leave 50 GB free for the build to run.

```sh
sudo apt install gawk wget git diffstat unzip texinfo gcc build-essential chrpath socat \
     cpio python3 python3-pip python3-pexpect xz-utils debianutils iputils-ping python3-git \
     python3-jinja2 python3-subunit zstd liblz4-tool file locales libacl1 bmap-tools
pip3 install --user kas            # tested with kas 5.5
```

**Build:**

```sh
git clone https://github.com/JJonline-git/rovedashcam_assignment.git rove-isp-channel
cd rove-isp-channel
kas build kas/rpi4.yml
```

kas clones poky, meta-openembedded and meta-raspberrypi at the pinned commits, writes the build configuration and runs `bitbake isp-image`.

- **A first build** is 3,931 tasks. It took about 3.5 h on a 4-core, 9 GB laptop.
- **After changing the app**, run `kas build kas/rpi4.yml` again. Only the app and the image tasks rerun; everything else comes from the local sstate cache.
- **To compile only the app** (quick check, no new image): `kas shell kas/rpi4.yml -c "bitbake isp-channel"`, about 2 min.

**Output** (flash it as in section 1):
`build/tmp/deploy/images/raspberrypi4-64/isp-image-raspberrypi4-64.rootfs.wic.bz2`, plus a `.wic.bmap` file next to it.

`local.conf` contains only build-host tuning: thread counts and `rm_work`. Everything that changes the image is in the layer.

## 3. Boot and connect

1. With the power off, connect the OV5647 ribbon to the **CAMERA** connector (not DISPLAY). The blue side of the ribbon faces the Ethernet port.
2. Connect Ethernet, then power on. eth0 gets an address by DHCP (systemd-networkd). Find the Pi's IP in your router, or with `nmap -sn 192.168.1.0/24`.
3. Log in as `root` with no password. This is a lab image; see REPORT.md, section 8.
   - over SSH: `ssh root@<pi-ip>`;
   - or on the serial console: GPIO14 (TX) / GPIO15 (RX), 115200 8N1.

The service starts at boot and records to `/var/lib/isp-channel/seg-00000.mp4` … `seg-00009.mp4` in 60-second segments. After the 10th segment it overwrites the oldest one (loop recording), so the disk never fills.

```sh
systemctl status isp-channel
journalctl -u isp-channel -b -f         # controls, stream config, then [stats] every 5 s
dmesg | grep -iE "ov5647|unicam"        # sensor probe
cam -l                                  # libcamera's own tool, for checking the sensor alone
```

A healthy stats line (service default, `--raw full`):

```
[isp] [stats] vf 15.61 fps (drops 0) | raw 15.61 fps (drops 0) | cpu 7.1% | rss 20024 kB | enc rejects 0
```

**Copy recordings to your PC:**

```sh
scp root@<pi-ip>:/var/lib/isp-channel/seg-00000.mp4 .
ffprobe seg-00000.mp4                   # h264 High, 1920x1080
```

## 4. Run by hand

Stop the service first, because only one process can own the camera:

```sh
systemctl stop isp-channel
isp-channel --raw video --outdir /tmp --duration 600          # 1080p30 + raw, 10-minute run
isp-channel --raw full  --outdir /tmp                         # raw at full 2592x1944 (15.6 fps)
isp-channel --no-zero-copy --raw video --outdir /tmp          # copy path, for comparison
isp-channel --ae off --exposure 10000 --gain 2 --awb-mode daylight --outdir /tmp
isp-channel --raw video --udp <pc-ip>:5000 --outdir /tmp      # also stream RTP/H.264 to a PC
isp-channel --list-controls                                   # print the ControlInfoMap and exit
systemctl start isp-channel                                   # back to normal afterwards
```

`/tmp` is a RAM disk (tmpfs), so files there are lost at reboot.

| Option | Meaning |
|---|---|
| `--width/--height/--fps` | viewfinder, default 1920x1080@30 |
| `--raw full\|video\|off` | raw stream at the largest sensor mode (the brief asks for this; it caps the rate at 15.6 fps), at the mode matching the viewfinder (keeps 30 fps), or none |
| `--buffers N` | buffers per stream (default 6) |
| `--ae on\|off`, `--awb on\|off`, `--exposure US`, `--gain G`, `--brightness -1..1`, `--contrast C`, `--awb-mode NAME` | camera controls; each is checked against the camera's ControlInfoMap before it is used |
| `--bitrate`, `--segment`, `--outdir` | encoder bitrate (default 8 Mbit/s), MP4 segment length (default 60 s), output folder |
| `--max-files N` | loop recording: keep the newest N segments and overwrite the oldest (default 10, about 600 MB; 0 = keep all) |
| `--udp HOST:PORT` | also send the H.264 stream as RTP over UDP |
| `--no-zero-copy` | the encoder copies frames instead of importing the dmabuf |
| `--stats SEC`, `--duration SEC` | stats interval, and stop after a set time |
| `--trigger-file PATH` | take a snapshot when this file appears (default `/run/isp-channel/snapshot`) |

The service reads its arguments from `/etc/default/isp-channel` (`ISP_CHANNEL_ARGS=...`). Edit it, then run `systemctl restart isp-channel`.

**Exit codes:** 0 = clean stop (SIGINT/SIGTERM or `--duration`), 1 = setup error (camera, configuration, options), 2 = GStreamer error (e.g. disk full), 3 = no frame for 5 s (sensor or CSI link stalled). systemd restarts the service on failure, backing off from 2 s to 60 s.

### Snapshot

```sh
touch /run/isp-channel/snapshot           # or: kill -USR1 $(pidof isp-channel)
```

Each snapshot writes three files in the output folder:
- `snap-NNNNNN.png`: the processed frame, converted from YUV to RGB in our code;
- `snap-NNNNNN.raw`: the raw Bayer frame, bit-exact;
- `snap-NNNNNN.txt`: format, size, stride, sequence number, exposure and gain.

### Watch the live stream on a PC

Start the viewer on the PC first, then start the app on the Pi with `--udp <pc-ip>:5000`:

```sh
ffplay -fflags nobuffer -flags low_delay -protocol_whitelist file,udp,rtp scripts/rtp-h264.sdp
# or: gst-launch-1.0 udpsrc port=5000 caps="application/x-rtp,media=video,encoding-name=H264,payload=96" \
#       ! rtph264depay ! avdec_h264 ! autovideosink
```

The stream is plain RTP over UDP (no RTSP), so the SDP file tells the viewer the port (5000) and the payload type (96 = H.264). If the firewall blocks it, allow UDP port 5000.

## 5. Check the "no V4L2 in userspace" rule

```sh
./scripts/check-no-v4l2.sh      # prints OK or lists the offending lines; exit code 0 = OK
```

V4L2 is used, but only *inside* libcamera (talking to Unicam and the ISP) and *inside* GStreamer's `v4l2h264enc` (talking to the encoder). See REPORT.md, section 3.

## 6. Repository layout

```
kas/rpi4.yml                      the whole build: machine, distro, layers pinned by commit
meta-isp-assignment/              our layer
  conf/layer.conf
  conf/distro/isp.conf            distro "isp": poky + systemd, no x11/wayland/gl, ov5647.dtbo
  recipes-core/images/isp-image.bb          image = core-image-minimal + app + ssh + networking
  recipes-bsp/bootfiles/rpi-config_git.bbappend   config.txt: gpu_mem, CMA, camera overlay, UART
  recipes-multimedia/libcamera/libcamera_%.bbappend  build only the Pi 4 pipeline (rpi/vc4)
  recipes-multimedia/gstreamer/*.bbappend    trim GStreamer plugin dependencies
  recipes-isp/isp-channel/
    isp-channel_1.0.bb            CMake app recipe + systemd unit
    isp-channel/isp-channel.service, isp-channel.default
    isp-channel/app/              the application (C11, plus one C++17 file for libcamera)
app -> meta-isp-assignment/recipes-isp/isp-channel/isp-channel/app   (shortcut)
scripts/check-no-v4l2.sh          checks the "no V4L2 in userspace" rule with grep
scripts/rtp-h264.sdp              for watching the live UDP stream on a PC
evidence/                         logs captured on the board (soak test, feature tests, faults)
```

## 7. Known limitations

- **Full-resolution raw and 30 fps conflict.** The OV5647's 2592x1944 mode runs at 15.6 fps at most, so `--raw full` (the service default, as the brief asks) runs the viewfinder at 15.6 fps. `--raw video` keeps 30 fps. Details are in REPORT.md, section 4.
- **The hardware encoder is limited to 1080p30 (H.264 level 4.0).**
- **Storage:** the root filesystem is about 1.5 GB whatever the SD card size (it is not grown on first boot). Loop recording keeps the recordings to about 600 MB.
- **The bonus (our own software ISP compared with the hardware ISP) is not done.** The raw dumps needed for it are already produced.
- **Lab-only image:** SSH with an empty root password. A product image would remove this.
