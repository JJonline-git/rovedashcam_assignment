/*
 * isp-channel - libcamera capture -> Raspberry Pi hardware ISP -> hardware H.264 -> mp4 / RTP.
 *
 * Threads:
 *   libcamera thread  : "Request completed"        -> queue inside camera.cpp
 *   GStreamer threads : "encoder done with buffer" -> cam_release() -> same queue
 *   main thread (here): cam_wait_frame() loop. The only thread that calls libcamera
 *                       (requeue) and pushes to the encoder.
 */
#define _GNU_SOURCE
#include <getopt.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <gst/gst.h>

#include "camera.h"
#include "encoder.h"
#include "log.h"
#include "snapshot.h"
#include "stats.h"

static volatile sig_atomic_t g_stop;
static volatile sig_atomic_t g_snapshot;

static void on_signal(int sig)
{
	/* Only set flags: almost nothing is safe to call inside a signal handler. */
	if (sig == SIGUSR1)
		g_snapshot = 1;
	else
		g_stop = 1;
}

/* The encoder calls this (from a GStreamer thread) when it no longer needs a frame. */
static void on_encoder_release(void *user, void *cookie)
{
	cam_release((struct cam *)user, cookie);
}

struct app_options {
	struct cam_options cam;
	struct encoder_options enc;
	unsigned duration_sec;             /* 0 = run until stopped */
	unsigned stats_sec;
	const char *trigger_file;
	int list_controls;
};

static void usage(void)
{
	printf("isp-channel [options]\n"
	       "  --camera ID            camera id (default: first)\n"
	       "  --width W --height H   viewfinder size (default 1920x1080)\n"
	       "  --fps N                frame rate (default 30)\n"
	       "  --raw full|video|off   raw stream: largest sensor mode / mode matching viewfinder / none\n"
	       "  --buffers N            buffers per stream (default 6)\n"
	       "  --ae on|off --awb on|off\n"
	       "  --exposure US --gain G --brightness B (-1..1) --contrast C (1.0 = normal)\n"
	       "  --awb-mode auto|incandescent|tungsten|fluorescent|indoor|daylight|cloudy|custom\n"
	       "  --bitrate BPS          H.264 bitrate (default 8000000)\n"
	       "  --outdir DIR           mp4 segments and snapshots (default .)\n"
	       "  --segment SEC          mp4 segment length (default 60)\n"
	       "  --udp HOST:PORT        also stream RTP/H.264 over UDP\n"
	       "  --no-zero-copy         encoder copies frames instead of importing the dmabuf\n"
	       "  --duration SEC         stop after SEC seconds (default: run forever)\n"
	       "  --stats SEC            stats interval (default 5)\n"
	       "  --trigger-file PATH    snapshot when this file appears (default /run/isp-channel/snapshot)\n"
	       "  --list-controls        print the camera's ControlInfoMap and exit\n"
	       "Snapshot: kill -USR1 <pid>  or  touch <trigger-file>\n");
}

static int on_off(const char *s)
{
	return !strcmp(s, "on") || !strcmp(s, "1");
}

static int parse_args(int argc, char **argv, struct app_options *o)
{
	enum { CAMERA = 256, WIDTH, HEIGHT, FPS, RAW, BUFFERS, AE, AWB, EXPOSURE, GAIN, BRIGHTNESS,
	       CONTRAST, AWBMODE, BITRATE, OUTDIR, SEGMENT, UDP, NOZC, DURATION, STATS, TRIGGER, LIST };
	static const struct option opts[] = {
		{ "camera", required_argument, NULL, CAMERA },     { "width", required_argument, NULL, WIDTH },
		{ "height", required_argument, NULL, HEIGHT },     { "fps", required_argument, NULL, FPS },
		{ "raw", required_argument, NULL, RAW },           { "buffers", required_argument, NULL, BUFFERS },
		{ "ae", required_argument, NULL, AE },             { "awb", required_argument, NULL, AWB },
		{ "exposure", required_argument, NULL, EXPOSURE }, { "gain", required_argument, NULL, GAIN },
		{ "brightness", required_argument, NULL, BRIGHTNESS },
		{ "contrast", required_argument, NULL, CONTRAST }, { "awb-mode", required_argument, NULL, AWBMODE },
		{ "bitrate", required_argument, NULL, BITRATE },   { "outdir", required_argument, NULL, OUTDIR },
		{ "segment", required_argument, NULL, SEGMENT },   { "udp", required_argument, NULL, UDP },
		{ "no-zero-copy", no_argument, NULL, NOZC },       { "duration", required_argument, NULL, DURATION },
		{ "stats", required_argument, NULL, STATS },       { "trigger-file", required_argument, NULL, TRIGGER },
		{ "list-controls", no_argument, NULL, LIST },      { "help", no_argument, NULL, 'h' },
		{ NULL, 0, NULL, 0 },
	};
	static char udp_host[128];
	int ch;

	/* Defaults */
	memset(o, 0, sizeof(*o));
	o->cam.width = 1920; o->cam.height = 1080; o->cam.fps = 30;
	o->cam.raw = CAM_RAW_FULL;
	/* 6 buffers: the encoder holds 2-3 frames, the camera needs >= 2 queued, 1 is with us. */
	o->cam.buffer_count = 6;
	o->enc.bitrate = 8000000;
	o->enc.outdir = ".";
	o->enc.segment_sec = 60;
	o->enc.udp_port = 5000;
	o->enc.zero_copy = 1;
	o->stats_sec = 5;
	o->trigger_file = "/run/isp-channel/snapshot";

	while ((ch = getopt_long(argc, argv, "h", opts, NULL)) != -1) {
		switch (ch) {
		case CAMERA:     o->cam.camera_id = optarg; break;
		case WIDTH:      o->cam.width = atoi(optarg); break;
		case HEIGHT:     o->cam.height = atoi(optarg); break;
		case FPS:        o->cam.fps = atoi(optarg); break;
		case RAW:
			if (!strcmp(optarg, "full"))       o->cam.raw = CAM_RAW_FULL;
			else if (!strcmp(optarg, "video")) o->cam.raw = CAM_RAW_VIDEO;
			else if (!strcmp(optarg, "off"))   o->cam.raw = CAM_RAW_OFF;
			else { ERR("--raw must be full|video|off"); return -1; }
			break;
		case BUFFERS:    o->cam.buffer_count = atoi(optarg); break;
		case AE:         o->cam.has_ae = 1; o->cam.ae = on_off(optarg); break;
		case AWB:        o->cam.has_awb = 1; o->cam.awb = on_off(optarg); break;
		case EXPOSURE:   o->cam.has_exposure = 1; o->cam.exposure_us = atoi(optarg); break;
		case GAIN:       o->cam.has_gain = 1; o->cam.gain = atof(optarg); break;
		case BRIGHTNESS: o->cam.has_brightness = 1; o->cam.brightness = atof(optarg); break;
		case CONTRAST:   o->cam.has_contrast = 1; o->cam.contrast = atof(optarg); break;
		case AWBMODE:    o->cam.awb_mode = optarg; break;
		case BITRATE:    o->enc.bitrate = atoi(optarg); break;
		case OUTDIR:     o->enc.outdir = optarg; break;
		case SEGMENT:    o->enc.segment_sec = atoi(optarg); break;
		case UDP: {
			char *colon = strrchr(optarg, ':');
			if (!colon || (size_t)(colon - optarg) >= sizeof(udp_host)) {
				ERR("--udp needs HOST:PORT");
				return -1;
			}
			memcpy(udp_host, optarg, colon - optarg);
			udp_host[colon - optarg] = '\0';
			o->enc.udp_host = udp_host;
			o->enc.udp_port = atoi(colon + 1);
			break;
		}
		case NOZC:       o->enc.zero_copy = 0; break;
		case DURATION:   o->duration_sec = atoi(optarg); break;
		case STATS:      o->stats_sec = atoi(optarg); break;
		case TRIGGER:    o->trigger_file = optarg; break;
		case LIST:       o->list_controls = 1; break;
		default:         usage(); return -1;
		}
	}
	if (!o->cam.fps || !o->stats_sec) {
		ERR("--fps and --stats must be > 0");
		return -1;
	}
	o->enc.width = o->cam.width;
	o->enc.height = o->cam.height;
	o->enc.fps = o->cam.fps;
	return 0;
}

/* Turn a camera frame into an encoder frame (no pixel is copied here). */
static void to_encoder_frame(const struct cam *c, const struct cam_frame *f, struct encoder_frame *ef)
{
	struct cam_stream_info vi;
	cam_stream_info(c, 0, &vi);
	memset(ef, 0, sizeof(*ef));
	ef->fd = f->vf.plane[0].fd;
	ef->cpu = f->vf.plane[0].cpu - f->vf.plane[0].offset;   /* start of the dmabuf mapping */
	for (unsigned i = 0; i < f->vf.num_planes && i < 3; i++) {
		size_t end = f->vf.plane[i].offset + f->vf.plane[i].length;
		ef->offset[i] = f->vf.plane[i].offset;
		if (end > ef->size)
			ef->size = end;
	}
	ef->stride[0] = vi.stride;           /* libcamera's YUV420 stride is the luma stride */
	ef->stride[1] = ef->stride[2] = vi.stride / 2;
	ef->sensor_ts_ns = f->vf.timestamp_ns;
	ef->cookie = f->handle;
}

int main(int argc, char **argv)
{
	struct app_options o;
	struct sigaction sa;
	int rc = 0;

	/* stdout to journald is a pipe; without line buffering logs arrive in 4 KB chunks. */
	setvbuf(stdout, NULL, _IOLBF, 0);

	if (parse_args(argc, argv, &o))
		return 1;
	gst_init(&argc, &argv);

	memset(&sa, 0, sizeof(sa));
	sa.sa_handler = on_signal;
	sigaction(SIGINT, &sa, NULL);
	sigaction(SIGTERM, &sa, NULL);      /* systemctl stop */
	sigaction(SIGUSR1, &sa, NULL);      /* snapshot */

	struct cam *cam = cam_open(&o.cam, o.list_controls);
	if (!cam)
		return 1;
	if (o.list_controls) {
		cam_close(cam);
		return 0;
	}

	struct encoder *enc = encoder_start(&o.enc, on_encoder_release, cam);
	if (!enc || cam_start(cam)) {
		encoder_stop(enc);
		cam_close(cam);
		return 1;
	}
	LOG("running: %s %ux%u@%u raw=%s zero-copy=%s", cam_model(cam), o.cam.width, o.cam.height,
	    o.cam.fps, o.cam.raw == CAM_RAW_FULL ? "full" : o.cam.raw == CAM_RAW_VIDEO ? "video" : "off",
	    o.enc.zero_copy ? "yes" : "no");

	struct stream_counter vf_count, raw_count;
	struct timing proc_time, sensor_to_app, latency;
	struct proc_sampler proc;
	struct cam_frame last = { 0 };
	uint64_t enc_rejects = 0;
	unsigned snap_index = 0;
	long rss_kb = 0, rss_baseline = 0;
	double cpu = 0;
	char b1[96], b2[96], b3[96];
	const uint64_t t0 = monotonic_ns();
	uint64_t last_stats = t0;

	counter_init(&vf_count);
	counter_init(&raw_count);
	timing_reset(&proc_time);
	timing_reset(&sensor_to_app);
	proc_init(&proc);

	while (!g_stop) {
		struct cam_frame f;

		if (cam_wait_frame(cam, &f, 100) == 1) {
			counter_frame(&vf_count, f.vf.sequence);
			if (f.has_raw)
				counter_frame(&raw_count, f.raw.sequence);
			timing_add(&sensor_to_app, (double)(f.completed_ns - f.vf.timestamp_ns) / 1000.0);
			last = f;

			if (!f.vf.ok) {
				cam_release(cam, f.handle);          /* bad frame: straight back */
			} else {
				if (g_snapshot || access(o.trigger_file, F_OK) == 0) {
					g_snapshot = 0;
					unlink(o.trigger_file);
					if (snapshot_busy())
						WARN("previous snapshot still being written, trigger ignored");
					else
						snapshot_submit(snapshot_copy(cam, &f, o.enc.outdir, snap_index++));
				}

				/* The Request goes back to the camera only when GStreamer releases the
				 * buffer (on_encoder_release). If the encoder refuses it, release now. */
				struct encoder_frame ef;
				to_encoder_frame(cam, &f, &ef);
				if (encoder_push(enc, &ef) < 0) {
					enc_rejects++;
					cam_release(cam, f.handle);
				}
				timing_add(&proc_time, (double)(monotonic_ns() - f.completed_ns) / 1000.0);
			}
		}

		if (encoder_check_bus(enc) < 0) {
			rc = 2;
			break;
		}

		uint64_t now = monotonic_ns();
		if (now - last_stats >= (uint64_t)o.stats_sec * 1000000000ull) {
			double sec = (double)(now - last_stats) / 1e9;
			proc_sample(&proc, &cpu, &rss_kb);
			if (!rss_baseline)
				rss_baseline = rss_kb;               /* first window = warmed-up baseline */
			encoder_take_latency(enc, &latency);
			LOG("[stats] vf %.2f fps (drops %llu) | raw %.2f fps (drops %llu) | cpu %.1f%% | "
			    "rss %ld kB | enc rejects %llu",
			    vf_count.frames / sec, (unsigned long long)vf_count.drops,
			    raw_count.frames / sec, (unsigned long long)raw_count.drops,
			    cpu, rss_kb, (unsigned long long)enc_rejects);
			LOG("[stats] app processing %s | sensor->app %s | sensor->encoded %s",
			    timing_str(&proc_time, b1, sizeof(b1)), timing_str(&sensor_to_app, b2, sizeof(b2)),
			    timing_str(&latency, b3, sizeof(b3)));
			/* What AE/AWB did on the latest frame (copied out at completion time). */
			LOG("[meta] exposure %d us | gain %.2f | colour gains R %.2f B %.2f | %d K | lux %.0f",
			    last.exposure_us, last.analogue_gain, last.colour_gain_r, last.colour_gain_b,
			    last.colour_temp_k, last.lux);
			counter_reset_window(&vf_count);
			counter_reset_window(&raw_count);
			timing_reset(&proc_time);
			timing_reset(&sensor_to_app);
			last_stats = now;
		}

		if (o.duration_sec && now - t0 >= (uint64_t)o.duration_sec * 1000000000ull)
			break;
	}

	/*
	 * Shutdown order matters:
	 *  1. cam_stop: no new frames; queued Requests come back cancelled.
	 *  2. encoder_stop: EOS finalises the mp4; NULL state drops every dmabuf reference.
	 *  3. cam_close: munmap and free the buffers. Safe now: nobody references them.
	 */
	LOG("stopping");
	cam_stop(cam);
	encoder_stop(enc);
	snapshot_wait();
	proc_sample(&proc, &cpu, &rss_kb);
	LOG("[summary] %.0f s | vf frames %llu drops %llu | raw frames %llu drops %llu | "
	    "rss baseline %ld kB end %ld kB (growth %ld kB)",
	    (double)(monotonic_ns() - t0) / 1e9,
	    (unsigned long long)vf_count.total_frames, (unsigned long long)vf_count.total_drops,
	    (unsigned long long)raw_count.total_frames, (unsigned long long)raw_count.total_drops,
	    rss_baseline, rss_kb, rss_kb - rss_baseline);
	cam_close(cam);
	return rc;
}
