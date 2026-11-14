# REPORT: Camera ISP channel on Raspberry Pi 4 (Yocto)

**Author:** Jenish J
**Track:** H, Raspberry Pi 4 + OV5647 (Sunny P5V04A)
**Status:** everything the brief requires runs on hardware, except the optional bonus (own software ISP).

## 0. Deliverables

| Item | Where |
|---|---|
| Source | this repository |
| Image | `isp-image-raspberrypi4-64.rootfs-20261114053835.wic.bz2` (78 MB) |
| Image SHA-256 | `95d029d490ffc213037a0c5ed14f983491b3a81c22a569982c8ebfb75d53bdb1` |
| Image download | GitHub release [`v1.0`](https://github.com/JJonline-git/rovedashcam_assignment/releases/tag/v1.0) |
| Screen recording | linked from the GitHub release (2–3 min) |
| Board logs | `evidence/` (soak test, feature tests, board snapshot, hardware fault trail) |

`kas build kas/rpi4.yml` rebuilds the image from scratch; see README.md.

---

## 1. What was built

```
 ┌──────────────── Raspberry Pi 4 ────────────────────────────────────────────────────────────┐
 │                                                                                             │
 │  OV5647 ──CSI-2, 2 lanes──▶ Unicam ──raw Bayer (dmabuf)──┬──────────────────────────────┐   │
 │    ▲  (sensor, 10-bit)      (CSI-2 receiver)              │                              │   │
 │    │ I2C: exposure, gain                                  ▼                              │   │
 │    │                                        bcm2835-isp (hardware ISP)                   │   │
 │    │                                        black level, lens shading, demosaic, AWB     │   │
 │    │                                        gains, CCM, gamma, denoise, scale → YUV420   │   │
 │    │                                              │ statistics          │ YUV (dmabuf)   │   │
 │  ┌─┴──────────────────────────────────────────────▼─────────────────────┼────────────────┼─┐ │
 │  │ libcamera 0.4.0, pipeline handler rpi/vc4, IPA rpi/vc4 + ov5647.json │  (kernel V4L2  │ │ │
 │  │ AGC/AEC, AWB, ALSC, …: decides the settings for each frame           │   used HERE,   │ │ │
 │  └──────────────────────────────────────────────────────────────────────┼─  not in app) ─┼─┘ │
 │                      Request completed: { viewfinder buffer, raw buffer, metadata }     │   │
 │  ┌──────────────────────────────────────────────────────────────────────▼────────────────▼─┐ │
 │  │ isp-channel (our app)                                                                   │ │
 │  │  camera.cpp (C++, libcamera API) ⇄ camera.h (C interface) ⇄ main.c, encoder.c,          │ │
 │  │  snapshot.c, stats.c (C11)                                                              │ │
 │  │   • viewfinder dmabuf fd ──wrapped, not copied──▶ GstBuffer                             │ │
 │  │   • trigger → snapshot thread: YUV→RGB PNG + raw Bayer dump + .txt                      │ │
 │  │   • stats every 5 s: fps, drops, CPU, RSS, latencies, AE/AWB metadata                   │ │
 │  └───────────────────────────────┬─────────────────────────────────────────────────────────┘ │
 │                                  ▼                                                           │
 │  appsrc → v4l2h264enc (bcm2835-codec = VideoCore hardware H.264, imports the dmabuf)         │
 │         → h264parse → tee ─┬─▶ queue → splitmuxsink → /var/lib/isp-channel/seg-NNNNN.mp4    │
 │                            └─▶ queue → rtph264pay → udpsink   (optional, --udp HOST:PORT)    │
 │                                                                                              │
 │  systemd: isp-channel.service (Restart=on-failure with backoff)                              │
 └──────────────────────────────────────────────────────────────────────────────────────────────┘
```

### Threads and the life of a buffer

```
 libcamera thread      requestCompleted() → push the Request on the "completed" queue, notify
 main thread           cam_wait_frame(): takes a completed Request, copies its metadata,
                       requeues any Requests that GStreamer has released, then pushes the
                       viewfinder fd to the encoder. All libcamera calls are made here.
 GStreamer threads     when the encoder has finished with a buffer, the GstBuffer is freed →
                       qdata destroy-notify → cam_release() → the Request goes on the
                       "released" queue (under a mutex) → the main thread requeues it.
 snapshot thread       writes the PNG and raw files from private copies (never holds a Request).
```

**Request recycling:**
- There are 6 Requests per run, created once and reused with `Request::reuse(ReuseBuffers)`.
- A buffer goes back to the camera only when the encoder is finished with it. This is what makes zero-copy safe: the ISP can never overwrite a frame the encoder is still reading.
- If the encoder's queue is full (4 frames waiting), that frame is not sent to the encoder. Its Request goes straight back to the camera, and the frame is counted as an `enc reject`. The camera loop never blocks.

### Copies

| Path | Copies | Why |
|---|---|---|
| sensor → ISP → app → encoder (default) | **0** | the dmabuf fd is shared; the CPU never touches pixels (`output-io-mode=dmabuf-import`, `GST_FD_MEMORY_FLAG_DONT_CLOSE`) |
| `--no-zero-copy` | 1 | inside `v4l2h264enc`, into its own MMAP input buffers (kept as a fallback and to measure the difference) |
| snapshot, YUV | 1 | the camera buffer is released quickly; PNG encoding takes over 100 ms. Each row is copied without stride padding. |
| snapshot, raw | 1 | same reason; keeps the Bayer data bit-exact |
| ISP → snapshot (CPU read) | n/a | `DMA_BUF_IOCTL_SYNC` start/end around the CPU read, so the cache is coherent with the ISP's DMA writes. This is a dma-buf ioctl, not V4L2. |

---

## 2. Yocto design

| Decision | Why |
|---|---|
| **kas** with all 4 repos pinned by commit (`kas/rpi4.yml`) | One command rebuilds the same image. |
| Our own **distro** `isp` (`conf/distro/isp.conf`), not local.conf | systemd as init, and x11/wayland/opengl/vulkan/ptest removed. Policy belongs in a distro file; local.conf holds only host tuning (threads, `rm_work`). |
| **`isp-image.bb` = `core-image-minimal`** + app, libcamera, kernel-modules, `systemd-conf`, dropbear | The smallest image that boots to a recording camera, with SSH and DHCP networking. The rootfs uses 195 MB, with 1 GiB extra for video. |
| `KERNEL_DEVICETREE:append:rpi = " overlays/ov5647.dtbo"` | meta-raspberrypi only ships the overlays for the IMX219/477/708. Without this line, `dtoverlay=ov5647` silently does nothing. |
| `rpi-config` bbappend: `camera_auto_detect=0`, `dtoverlay=ov5647` | Binds the sensor explicitly, so boot does not depend on auto-probing a third-party module. `VIDEO_CAMERA` is *not* used: it sets `start_x=1`, the legacy firmware camera stack, which conflicts with libcamera. |
| `GPU_MEM = "128"` | With KMS the camera and display buffers come from Linux CMA, not GPU memory. The hardware H.264 encoder, however, still runs as VideoCore firmware. 128 MB is Raspberry Pi's documented value for camera + encode. |
| `VC4DTBO = "vc4-kms-v3d,cma-256"` | Budget: viewfinder 6 × 3.1 MB + raw 6 × 6.3 MB + ISP and encoder buffers ≈ 90 MB. **Measured: 88 MB in use** (CmaTotal 262144 kB, CmaFree 171656 kB, raw full + encoder running). 256 MB leaves about 2.5× headroom. |
| `LIBCAMERA_PIPELINES:rpi = "rpi/vc4"` | Builds only the Pi 4 pipeline handler and IPA, not every platform. |
| GStreamer bbappends (`PACKAGECONFIG` trimmed) | The default plugins-bad config pulls in librsvg, and with it the Rust toolchain: hours of build time for nothing we use. We keep only `v4l2` + `gudev` in plugins-good. |
| App recipe: CMake, `inherit cmake pkgconfig systemd`, RDEPENDS on the exact GStreamer plugin packages | The image contains only the 8 plugins the pipeline uses. |
| systemd unit: `Restart=on-failure`, `RestartSteps=5`, `RestartMaxDelaySec=60`, `StateDirectory`, `RuntimeDirectory`, `TimeoutStopSec=15` | Restarts back off from 2 s to 60 s; this was added after a real restart storm (section 6). systemd creates the recording and trigger folders. SIGTERM → EOS, so the last MP4 is finalised. |

**Build facts:**
- Yocto 5.0.21 (Scarthgap), BitBake 2.8.1, kernel 6.6.63-v8.
- libcamera 0.4.0, GStreamer 1.22.12.
- First build: 3,931 tasks, about 3.5 h on 4 cores.
- The public sstate mirror was tried and dropped: from this network yoctoproject.org downloaded at 40–70 KB/s, slower than compiling locally.

---

## 3. The "no V4L2 in userspace" constraint

- **Our app uses only:** the libcamera C++ API (`camera.cpp`), the GStreamer API (`encoder.c`), libpng, and the dma-buf cache-sync ioctl.
- **Check:** `scripts/check-no-v4l2.sh` greps the app for `videodev2.h`, `VIDIOC_` and `v4l2-ctl`, and prints **OK**. It also fails if grep itself fails, so it can never pass silently.
- **Where V4L2 is still used:**
  - inside libcamera, which drives Unicam and the ISP through the kernel's V4L2/media-controller interface;
  - inside GStreamer's `v4l2h264enc`, the only way to reach the Pi 4's hardware encoder.

  Both are frameworks, not our code. That is how I read the rule's intent: the app goes through the camera framework, and never opens video devices itself.

---

## 4. The conflict in the brief: full-resolution raw vs 1080p30

The brief asks for 1080p30 YUV **and** raw "at full sensor mode" at the same time. The OV5647 can't do both:

| Sensor mode | Max fps |
|---|---|
| 2592x1944 (full) | **15.63** (measured) |
| 1920x1080 | 30 (measured) |
| 1296x972 (binned) | ~46 (kernel driver mode table, not measured) |
| 640x480 | ~58 (kernel driver mode table, not measured) |

Both streams come from the same sensor readout, so with full-mode raw the viewfinder also runs at 15.63 fps.

**What I did:**
- **An option, `--raw full|video|off`:**
  - `full` (the service default) follows the brief literally: raw at 2592x1944, both streams at 15.63 fps.
  - `video` picks the smallest mode that covers the viewfinder (1920x1080), so both streams run at 30 fps.
  - `off` drops the raw stream.
- **For a dash cam I would ship `video`:** smooth 30 fps recording matters more than 5 MP raw frames. I would get full-resolution stills by briefly switching modes on a trigger. That adds a mode-switch delay of a few frames; see section 9.

---

## 5. Measurements

All on a Pi 4 (Rev 1.4, 8 GB) with the OV5647, 1920x1080 YUV420 viewfinder, H.264 at 8 Mbit/s.
- "CPU" is the app process (all its threads, including libcamera's IPA and GStreamer), as % of **one** core, from `/proc/self/stat`.
- "RSS" comes from `/proc/self/statm`.
- Raw logs are in `evidence/`.

| Run | Viewfinder FPS | Raw FPS | Drops | CPU | RSS start → end | App time per frame (avg / max) | Sensor → app (avg) | **Sensor → encoded (avg / max)** |
|---|---|---|---|---|---|---|---|---|
| **Soak, 600 s, zero-copy, `--raw video`** | **30.00** | 30.00 | **0 / 17,995** | **14 %** | 19,520 → 19,776 kB (**+256 kB**) | see note 1 | ~46 ms | ~89 ms |
| 20 s, zero-copy, `--raw video` | 29.99 | 29.99 | 0 / 592 | 13.9 % | 19,776 → 19,776 kB | 158 µs / 218 µs | 46.1 ms | 89.5 / 95.9 ms |
| 20 s, **copy path**, `--raw video` | 30.00 | 30.00 | 0 / 592 | **34.6 %** | 19,904 → 19,904 kB | 163 µs / 588 µs | 45.2 ms | 89.5 / 98.0 ms |
| 20 s, zero-copy, **`--raw full`** + 1 snapshot | 15.63 | 15.63 | 0 / 305 | 6.9 % | 19,520 → 23,328 kB (snapshot, see note 2) | 194 µs / 458 µs | 92.1 ms | 131.8 / 141.7 ms |

Notes:
1. In the soak run the stats lines were sampled every 5 s; every window shows 30.00 fps, 0 drops and 13.9–14.6 % CPU. The first window reads 28.4 fps only because it includes start-up.
2. The +3.8 MB in the `--raw full` run comes from the one snapshot: libpng/zlib state and the snapshot thread's stack, allocated once. RSS stays flat afterwards.

**How to read the numbers:**
- **Zero-copy saves about 20 percentage points of a core** (34.6 % → 13.9 %). That is the cost of `v4l2h264enc` copying 3.1 MB per frame, 30 times a second (≈ 93 MB/s of memcpy).
- **App time per frame is about 0.16 ms out of a 33 ms frame budget**, measured from Request completion until the buffer is in the encoder queue. The app is not the bottleneck.
- **Sensor → app** is measured from the frame's sensor timestamp (start of frame, CLOCK_MONOTONIC) to Request completion.
  - At 30 fps, ~46 ms ≈ one frame readout (33 ms) + the ISP pass.
  - At 15.6 fps, ~92 ms ≈ one 64 ms readout + the ISP pass.
  - This is the camera stack's own latency, not ours.
- **Sensor → encoded** is measured with a buffer probe on the encoder's output pad, using the same clock. It adds about 44 ms for the hardware encoder at 30 fps.
  - This is the "end-to-end" number I can measure on the device.
  - Glass-to-glass latency (a light flash to the decoded picture on another device) would also include network, decode and display; it was not measured.
- **Memory is flat:** +256 kB over 10 minutes, all in the first minute (GStreamer and libcamera internal caches warming up). No leak in the Request/buffer cycle.
- **The encoder never fell behind** (`enc rejects 0` in every run).

Other checks (details in `evidence/feature-tests.txt`):
- MP4 output: H.264 High profile, level 4.0, 1920x1080, 30/1 (checked with ffprobe).
- Snapshot: 1920x1080 RGB PNG + 6,345,216-byte raw frame + metadata file.
- Manual controls: AE off, exposure 10 ms, gain 2.0, AWB daylight, brightness 0.1 and contrast 1.2 were applied. The per-frame metadata reads back exposure 9,995 µs (whole sensor line periods) and gain 2.00.
- An unsupported AWB mode is rejected with a warning.
- RTP/UDP: about 7.9 Mbit/s received on the laptop.
- The service records automatically at boot.

**ControlInfoMap** (logged at start; `--list-controls` prints only this). 22 controls, among them:
- `AeEnable`, `ExposureTime [0..66666]`, `AnalogueGain [1..16]`, `AeExposureMode`, `AeMeteringMode`, `AeConstraintMode`, `ExposureValue [-8..8]`, `AeFlickerMode`, `AeFlickerPeriod`;
- `AwbEnable`, `AwbMode [0..7]`, `ColourGains`, `ColourTemperature`;
- `Brightness [-1..1]`, `Contrast [0..32]`, `Saturation`, `Sharpness`;
- `FrameDurationLimits [33333..120000]`, `ScalerCrop`, `NoiseReductionMode`, `HdrMode`, `StatsOutputEnable`.

---

## 6. Debugging on hardware

The image built and booted first time, and the app compiled with no warnings. Four problems came up on the board. I treated each one by isolating the layer before changing anything. The full logs are in `evidence/hardware-fault.txt`.

**1. No frames: "Camera frontend has timed out!"**
- **Symptom:** the sensor probed over I2C, libcamera configured both streams, but Unicam received nothing on CSI-2 for 1 s.
- **Diagnosis:** I2C (control) worked but the CSI-2 data lanes were silent. That points to the physical link, not the software. libcamera itself says the same.
- **What happened next:** after the cable was reseated on a second Pi 4, frames arrived. Then problem 2 hid them.

**2. `not-negotiated` from appsrc (a software bug, fixed in two steps)**
- **Symptom:** 2 frames were captured, then GStreamer stopped with `not-negotiated`.
- **Isolation, without the camera:**
  - I fed `/dev/zero` frames into the same encoder with `gst-launch`, so the camera was out of the picture.
  - This showed the capsfilter could not fix `colorimetry`: the encoder's caps template lists several colorimetries.
  - Setting `colorimetry=1:4:7:1` made `gst-launch` encode. This is sYCC, what the ISP reports: full range, BT.601 matrix, sRGB transfer, BT.709 primaries.
- **Second missing field:** in the app the error remained. `GST_DEBUG=GST_CAPS:3` showed the encoder's accept-caps also lists `interlace-mode=progressive`, and caps missing a field the encoder lists are rejected. With both fields, it encoded.
- **Lesson:** `gst-launch` worked earlier because its capsfilter fills in defaults; `appsrc` sends exactly the caps you give it.

**3. Restart storm**
- **Symptom:** every run failed in about 1 s, and systemd's `RestartSec=3` restarted the app **720 times**. Each restart opened the sensor, streamed it and closed it again.
- **Fix:**
  - The unit now backs off from 2 s to 60 s (`RestartSteps`, `RestartMaxDelaySec`).
  - The app has a **stall watchdog**: it exits with code 3 if no frame arrives for 5 s. Before, it logged `0.00 fps` forever, which is the worst failure for a dash cam: it looks alive and records nothing.

**4. The sensor stopped answering on I2C**
- **Symptom:** after the storm, `i2c read error, reg: 100 = -5` (no ACK). After a warm reboot, `ID High expected 0x56 got 0`. After a power cycle, `-110` (bus timeout).
- **Isolation:** libcamera's own `cam` tool failed the same way, so our app was not involved.
- **Fix:** the camera, ribbon and both connectors were redone on the first Pi. After that the probe was clean and `cam` captured at 15 fps. All the measurements above are from that setup.
- **Conclusion:** an intermittent contact in the ribbon/connector path; the fault followed the cable handling, not a particular board. Repeated stream starts on a marginal link probably pushed the sensor into a bad state. A warm reboot does not power-cycle the camera rail, so only a full power-off cleared it.

Small fixes found along the way:
- The summary printed "rss baseline 0" for runs shorter than one stats window.
- BusyBox `head` needs `-n 80`, not `-80`.

---

## 7. Code structure (and why one C++ file)

- libcamera only has a C++ API. My background is C, so I isolated libcamera behind a small C interface and wrote everything else in C11:
  - `camera.h`: an opaque `struct cam *`, like `FILE *`, with `cam_open`, `cam_start`, `cam_wait_frame`, `cam_release`, `cam_stop` and `cam_close`;
  - `camera.cpp`: the only C++ file, about 490 lines, all the libcamera calls;
  - `main.c`: options, signals, the frame loop, stats, shutdown order;
  - `encoder.c`: the GStreamer pipeline and dmabuf wrapping; `snapshot.c`: PNG/raw writer thread; `stats.c`: fps/drops/latency/CPU/RSS.
- **Shutdown order:** `cam_stop` → `encoder_stop` (EOS, then NULL state, which drops every dmabuf reference) → `snapshot_wait` → `cam_close` (munmap, free the buffers). If the buffers were freed before the encoder let go of them, the result would be a use-after-free on DMA memory.
- **Signal handlers** only set `volatile sig_atomic_t` flags, which is async-signal-safe.
- **`camera.cpp` copies each frame's metadata when the frame is received**, never later. After `queueRequest` the Request belongs to libcamera again; an early draft read it later, and that bug was caught in review.
- All builds use `-Wall -Wextra`, with 0 warnings.

---

## 8. Assumptions

1. **The OV5647 replaces the IMX219.** The recruiter confirmed this by email.
   - It has the same pipeline handler (rpi/vc4) and IPA.
   - The differences are the tuning file (`ov5647.json`) and the sensor modes (section 4).
2. **The brief's "Raw at full sensor mode" is taken literally** as the service default (`--raw full`). `--raw video` is offered for 30 fps (section 4).
3. **"End-to-end latency" is measured from sensor timestamp to encoded H.264 buffer**, on the device. Glass-to-glass was not measured.
4. **"No V4L2 in userspace" applies to our application code**, not to libcamera or GStreamer internals (section 3).
5. **Container:** MP4 via `splitmuxsink` in 60 s segments. For a dash cam, a power cut then loses at most one segment. A single MP4 written until stop would be unreadable if power were lost before the `moov` atom is written.
6. **The image is for a lab:** root without a password over SSH, and a serial console. A product would use SSH keys or no SSH, a read-only rootfs, and a separate data partition for video.
7. **The H.264 settings are chosen for a dash cam:** 8 Mbit/s, High profile, level 4, an I-frame every 30 frames (1 s), and SPS/PPS repeated, so each segment and each stream join starts decoding quickly.

## 9. Not done / with more time

- **Bonus, own ISP vs hardware ISP: not done.** The inputs are ready: `snap-*.raw` (SGBRG10 CSI-2 packed, stride 3264) plus the hardware-ISP PNG of the same frame. The plan:
  1. unpack the 10-bit data and subtract the black level;
  2. bilinear demosaic;
  3. apply the AWB gains from the metadata, the CCM and gamma;
  4. compare with the hardware ISP output (PSNR and visual diff) and time it on the Cortex-A72 against the hardware ISP, which handles 30 fps with no CPU load.
- **Full-resolution stills while recording at 30 fps**, by a mode switch on trigger or a second configuration.
- **A camera health check:** detect the CSI-2 stall (problem 1) and power-cycle the sensor through its enable GPIO, instead of only exiting.
- **Glass-to-glass latency test** (an LED blinking in view, against a photodiode or a high-speed phone video).
- **Storage management:** delete the oldest segment when the disk is nearly full (loop recording), and a separate data partition.
- **RTSP server** (`gst-rtsp-server`) instead of raw RTP/UDP, for easier viewing.
