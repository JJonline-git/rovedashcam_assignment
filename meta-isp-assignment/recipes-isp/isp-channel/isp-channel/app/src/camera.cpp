/*
 * camera.cpp - the only C++ file. Wraps the libcamera C++ API behind the C functions in
 * camera.h. Written in a deliberately simple style. C++ features used, and what they mean
 * for a C programmer:
 *
 *   std::shared_ptr<T> / std::unique_ptr<T>  pointer that frees the object automatically
 *                                            (unique = one owner, shared = reference counted)
 *   std::vector<T>, std::map<K,V>, std::deque<T>   growable array, sorted map, queue
 *   std::mutex, std::condition_variable      = pthread_mutex_t, pthread_cond_t
 *   obj->method(args)                        = function(obj, args)
 *   T &ref                                   = a pointer that cannot be NULL, used without '*'
 *   new T / delete p                         = malloc + constructor / destructor + free
 *   namespace libcamera, "using namespace"   = prefix for names (libcamera::Camera -> Camera)
 */
#include "camera.h"
#include "log.h"

#include <libcamera/libcamera.h>

#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

#include <algorithm>
#include <condition_variable>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

using namespace libcamera;

static uint64_t now_ns(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint64_t)ts.tv_sec * 1000000000ull + ts.tv_nsec;
}

/* CPU mapping of one FrameBuffer: one mmap per distinct dmabuf fd. */
struct Mapping {
	std::vector<int> fds;
	std::vector<void *> addrs;
	std::vector<size_t> lens;
	std::vector<const uint8_t *> planes;   /* CPU pointer of each plane */
};

/* The hidden contents of "struct cam" (C only sees an opaque pointer). */
struct cam {
	std::unique_ptr<CameraManager> cm;
	std::shared_ptr<Camera> camera;
	std::unique_ptr<CameraConfiguration> config;
	std::unique_ptr<FrameBufferAllocator> allocator;
	std::vector<std::unique_ptr<Request>> requests;
	std::map<const FrameBuffer *, Mapping> maps;
	ControlList controls;
	Stream *vf = nullptr;
	Stream *raw = nullptr;
	std::string model;
	bool acquired = false;
	bool started = false;

	/* Two queues filled by other threads, emptied by cam_wait_frame() in the main thread. */
	std::mutex lock;
	std::condition_variable cond;
	std::deque<Request *> completed;   /* filled by libcamera's thread */
	std::deque<Request *> released;    /* filled by whoever calls cam_release() */
	std::map<Request *, uint64_t> completed_at;

	/* Called by libcamera in ITS thread when a Request finishes. Just queue it. */
	void on_request_completed(Request *req)
	{
		std::lock_guard<std::mutex> guard(lock);   /* locks now, unlocks at end of scope */
		completed.push_back(req);
		completed_at[req] = now_ns();
		cond.notify_one();
	}
};

/* ------------------------------------------------------------------ buffer mapping */

static bool map_buffer(const FrameBuffer *fb, Mapping &m)
{
	/* On the Pi, Y, U and V are one dmabuf at three offsets. Map each fd once,
	 * large enough to cover all of its planes. Done once at startup, never per frame. */
	for (const FrameBuffer::Plane &p : fb->planes()) {
		int fd = p.fd.get();
		size_t end = p.offset + p.length;
		size_t i;
		for (i = 0; i < m.fds.size(); i++)
			if (m.fds[i] == fd)
				break;
		if (i == m.fds.size()) {
			m.fds.push_back(fd);
			m.lens.push_back(end);
		} else if (end > m.lens[i]) {
			m.lens[i] = end;
		}
	}
	for (size_t i = 0; i < m.fds.size(); i++) {
		void *a = mmap(NULL, m.lens[i], PROT_READ, MAP_SHARED, m.fds[i], 0);
		if (a == MAP_FAILED) {
			ERR("mmap of dmabuf fd %d failed", m.fds[i]);
			return false;
		}
		m.addrs.push_back(a);
	}
	for (const FrameBuffer::Plane &p : fb->planes()) {
		for (size_t i = 0; i < m.fds.size(); i++)
			if (m.fds[i] == p.fd.get())
				m.planes.push_back((const uint8_t *)m.addrs[i] + p.offset);
	}
	return true;
}

static void fill_buffer(const cam *c, const FrameBuffer *fb, cam_buffer *out)
{
	const Mapping &m = c->maps.at(fb);
	const FrameMetadata &md = fb->metadata();
	out->num_planes = std::min<size_t>(fb->planes().size(), 3);
	for (unsigned i = 0; i < out->num_planes; i++) {
		const FrameBuffer::Plane &p = fb->planes()[i];
		out->plane[i].fd = p.fd.get();
		out->plane[i].offset = p.offset;
		out->plane[i].length = p.length;
		out->plane[i].cpu = m.planes[i];
	}
	out->sequence = md.sequence;
	out->timestamp_ns = md.timestamp;
	out->ok = md.status == FrameMetadata::FrameSuccess;
}

/* ------------------------------------------------------------------ controls */

static void log_controls(const cam *c)
{
	/* ControlInfoMap = every control this camera supports, with min/max/default. */
	const ControlInfoMap &info = c->camera->controls();
	LOG("supported controls (%zu):", info.size());
	for (const auto &entry : info)          /* entry.first = ControlId*, entry.second = ControlInfo */
		LOG("  %-28s %s", entry.first->name().c_str(), entry.second.toString().c_str());
}

static bool supported(const cam *c, const ControlId &id)
{
	const ControlInfoMap &info = c->camera->controls();
	if (info.find(&id) == info.end()) {
		WARN("control %s not supported by this camera, ignored", id.name().c_str());
		return false;
	}
	return true;
}

static void build_controls(cam *c, const cam_options *o)
{
	c->controls = ControlList(c->camera->controls());

	/* Min = max frame duration pins the frame rate. If the sensor mode cannot reach it
	 * (e.g. full-resolution raw), the IPA clamps it to what the mode can do. */
	int64_t frame_us = 1000000 / o->fps;
	if (supported(c, controls::FrameDurationLimits))
		c->controls.set(controls::FrameDurationLimits, Span<const int64_t, 2>({ frame_us, frame_us }));

	/* controls::X are typed: AeEnable is bool, ExposureTime int32 (us), AnalogueGain float... */
	if (o->has_ae && supported(c, controls::AeEnable)) {
		c->controls.set(controls::AeEnable, o->ae != 0);
		LOG("control AeEnable = %d", o->ae);
	}
	if (o->has_exposure && supported(c, controls::ExposureTime)) {
		c->controls.set(controls::ExposureTime, o->exposure_us);
		LOG("control ExposureTime = %d us", o->exposure_us);
	}
	if (o->has_gain && supported(c, controls::AnalogueGain)) {
		c->controls.set(controls::AnalogueGain, o->gain);
		LOG("control AnalogueGain = %.2f", o->gain);
	}
	if (o->has_awb && supported(c, controls::AwbEnable)) {
		c->controls.set(controls::AwbEnable, o->awb != 0);
		LOG("control AwbEnable = %d", o->awb);
	}
	if (o->has_brightness && supported(c, controls::Brightness)) {
		c->controls.set(controls::Brightness, o->brightness);
		LOG("control Brightness = %.2f", o->brightness);
	}
	if (o->has_contrast && supported(c, controls::Contrast)) {
		c->controls.set(controls::Contrast, o->contrast);
		LOG("control Contrast = %.2f", o->contrast);
	}

	if (o->awb_mode && supported(c, controls::AwbMode)) {
		static const struct { const char *name; int32_t value; } modes[] = {
			{ "auto", controls::AwbAuto },         { "incandescent", controls::AwbIncandescent },
			{ "tungsten", controls::AwbTungsten }, { "fluorescent", controls::AwbFluorescent },
			{ "indoor", controls::AwbIndoor },     { "daylight", controls::AwbDaylight },
			{ "cloudy", controls::AwbCloudy },     { "custom", controls::AwbCustom },
		};
		int32_t value = -1;
		for (const auto &m : modes)
			if (!strcmp(m.name, o->awb_mode))
				value = m.value;
		/* For enum controls, ControlInfo::values() lists the values the camera accepts. */
		bool offered = false;
		for (const ControlValue &v : c->camera->controls().at(&controls::AwbMode).values())
			if (v.get<int32_t>() == value)
				offered = true;
		if (value >= 0 && offered) {
			c->controls.set(controls::AwbMode, value);
			LOG("control AwbMode = %s", o->awb_mode);
		} else {
			WARN("AWB mode '%s' not offered by this camera", o->awb_mode);
		}
	}
}

/* ------------------------------------------------------------------ C API */

extern "C" struct cam *cam_open(const struct cam_options *o, int list_only)
{
	cam *c = new cam;

	/* 1. Enumerate. At boot the camera kernel modules may still be loading, so retry. */
	c->cm = std::make_unique<CameraManager>();
	if (c->cm->start()) {
		ERR("CameraManager start failed");
		delete c;
		return NULL;
	}
	for (int i = 0; i < 20 && c->cm->cameras().empty(); i++) {
		c->cm->stop();
		usleep(500000);
		c->cm->start();
	}
	std::vector<std::shared_ptr<Camera>> cams = c->cm->cameras();
	LOG("found %zu camera(s)", cams.size());
	for (const auto &cm_cam : cams)
		LOG("  %s", cm_cam->id().c_str());
	if (cams.empty()) {
		cam_close(c);
		return NULL;
	}

	c->camera = o->camera_id ? c->cm->get(o->camera_id) : cams[0];
	if (!c->camera || c->camera->acquire()) {   /* acquire = exclusive access */
		ERR("cannot get/acquire camera");
		cam_close(c);
		return NULL;
	}
	c->acquired = true;
	auto model = c->camera->properties().get(properties::Model);  /* std::optional: may be empty */
	c->model = model ? *model : c->camera->id();
	LOG("using %s", c->model.c_str());

	log_controls(c);
	if (list_only)
		return c;

	/* 2. Configure. Viewfinder = processed YUV from the hardware ISP; Raw = Bayer straight
	 *    from the CSI-2 receiver. The pipeline handler proposes a config, we edit it. */
	std::vector<StreamRole> roles = { StreamRole::Viewfinder };
	if (o->raw != CAM_RAW_OFF)
		roles.push_back(StreamRole::Raw);
	c->config = c->camera->generateConfiguration(roles);
	if (!c->config) {
		ERR("cannot generate configuration");
		cam_close(c);
		return NULL;
	}

	StreamConfiguration &vf = c->config->at(0);
	vf.pixelFormat = formats::YUV420;
	vf.size = Size(o->width, o->height);
	vf.bufferCount = o->buffer_count;

	if (o->raw != CAM_RAW_OFF) {
		StreamConfiguration &raw = c->config->at(1);
		/* The sizes offered for the raw stream are the sensor's native modes. */
		std::vector<Size> modes = raw.formats().sizes(raw.pixelFormat);
		std::string list;
		for (const Size &s : modes)
			list += s.toString() + " ";
		LOG("sensor modes (%s): %s", raw.pixelFormat.toString().c_str(), list.c_str());

		Size best(0, 0);
		for (const Size &s : modes) {
			uint64_t area = (uint64_t)s.width * s.height;
			uint64_t best_area = (uint64_t)best.width * best.height;
			if (o->raw == CAM_RAW_FULL) {
				if (area > best_area)
					best = s;                   /* largest mode */
			} else if (s.width >= o->width && s.height >= o->height &&
				   (best_area == 0 || area < best_area)) {
				best = s;                           /* smallest mode covering the viewfinder */
			}
		}
		if (best.width)
			raw.size = best;
		raw.bufferCount = o->buffer_count;
	}

	/* validate() may adjust things to what the hardware can do (alignment, sizes). */
	CameraConfiguration::Status st = c->config->validate();
	if (st == CameraConfiguration::Invalid) {
		ERR("configuration invalid");
		cam_close(c);
		return NULL;
	}
	if (st == CameraConfiguration::Adjusted)
		WARN("configuration adjusted by the pipeline handler");
	if (c->camera->configure(c->config.get()) < 0) {
		ERR("configure failed");
		cam_close(c);
		return NULL;
	}
	for (unsigned i = 0; i < c->config->size(); i++) {
		const StreamConfiguration &s = c->config->at(i);
		LOG("stream %u: %s stride %u frameSize %u buffers %u colorspace %s", i,
		    s.toString().c_str(), s.stride, s.frameSize, s.bufferCount,
		    s.colorSpace ? s.colorSpace->toString().c_str() : "n/a");
	}
	c->vf = c->config->at(0).stream();
	c->raw = c->config->size() > 1 ? c->config->at(1).stream() : nullptr;

	/* 3. Buffers: dmabuf-backed, from the driver's CMA memory. Map each one once. */
	c->allocator = std::make_unique<FrameBufferAllocator>(c->camera);
	size_t n_requests = SIZE_MAX;
	for (StreamConfiguration &s : *c->config) {
		if (c->allocator->allocate(s.stream()) < 0) {
			ERR("buffer allocation failed");
			cam_close(c);
			return NULL;
		}
		const auto &bufs = c->allocator->buffers(s.stream());
		n_requests = std::min(n_requests, bufs.size());
		for (const auto &b : bufs)
			if (!map_buffer(b.get(), c->maps[b.get()])) {
				cam_close(c);
				return NULL;
			}
	}

	/* 4. Requests: one buffer per stream each. Created once here and recycled forever,
	 *    so the frame loop never allocates memory. */
	for (size_t i = 0; i < n_requests; i++) {
		std::unique_ptr<Request> req = c->camera->createRequest(i);
		for (StreamConfiguration &s : *c->config)
			req->addBuffer(s.stream(), c->allocator->buffers(s.stream())[i].get());
		c->requests.push_back(std::move(req));
	}
	LOG("%zu requests created", c->requests.size());

	build_controls(c, o);
	/* Connect the libcamera signal to our method (like registering a callback). */
	c->camera->requestCompleted.connect(c, &cam::on_request_completed);
	return c;
}

extern "C" int cam_start(struct cam *c)
{
	/* Controls given to start() apply from the very first frame. */
	if (c->camera->start(&c->controls) < 0) {
		ERR("camera start failed");
		return -1;
	}
	c->started = true;
	for (auto &req : c->requests)
		if (c->camera->queueRequest(req.get()) < 0) {
			ERR("queueRequest failed");
			return -1;
		}
	return 0;
}

extern "C" const char *cam_model(const struct cam *c)
{
	return c->model.c_str();
}

extern "C" int cam_stream_info(const struct cam *c, int stream, struct cam_stream_info *info)
{
	Stream *s = stream == 0 ? c->vf : c->raw;
	if (!s)
		return -1;
	const StreamConfiguration &sc = s->configuration();
	info->width = sc.size.width;
	info->height = sc.size.height;
	info->stride = sc.stride;
	snprintf(info->pixel_format, sizeof(info->pixel_format), "%s", sc.pixelFormat.toString().c_str());
	info->rec709 = sc.colorSpace && sc.colorSpace->ycbcrEncoding == ColorSpace::YcbcrEncoding::Rec709;
	info->full_range = sc.colorSpace && sc.colorSpace->range == ColorSpace::Range::Full;
	return 0;
}

extern "C" void cam_release(struct cam *c, void *handle)
{
	/* May run in a GStreamer thread: only queue it. The main thread does the requeue,
	 * so every libcamera call happens in one thread. */
	std::lock_guard<std::mutex> guard(c->lock);
	c->released.push_back((Request *)handle);
	c->cond.notify_one();
}

extern "C" int cam_wait_frame(struct cam *c, struct cam_frame *f, int timeout_ms)
{
	uint64_t deadline = now_ns() + (uint64_t)timeout_ms * 1000000ull;

	for (;;) {
		std::vector<Request *> to_requeue;
		Request *req = nullptr;
		uint64_t completed_at = 0;
		{
			std::unique_lock<std::mutex> guard(c->lock);
			/* Like pthread_cond_timedwait in a loop, until something is queued. */
			c->cond.wait_for(guard, std::chrono::milliseconds(timeout_ms), [c] {
				return !c->completed.empty() || !c->released.empty();
			});
			to_requeue.assign(c->released.begin(), c->released.end());
			c->released.clear();
			if (!c->completed.empty()) {
				req = c->completed.front();
				c->completed.pop_front();
				completed_at = c->completed_at[req];
			}
		}   /* mutex unlocked here */

		/* Request recycling: same buffers, new capture. ReuseBuffers keeps them attached. */
		for (Request *r : to_requeue) {
			if (!c->started)
				continue;
			r->reuse(Request::ReuseBuffers);
			c->camera->queueRequest(r);
		}

		if (req) {
			if (req->status() == Request::RequestCancelled)
				continue;   /* only during stop() */
			memset(f, 0, sizeof(*f));
			f->handle = req;
			f->completed_ns = completed_at;
			fill_buffer(c, req->findBuffer(c->vf), &f->vf);
			if (c->raw) {
				fill_buffer(c, req->findBuffer(c->raw), &f->raw);
				f->has_raw = 1;
			}
			/* Copy the metadata NOW: after the Request is requeued, reuse() clears it. */
			const ControlList &md = req->metadata();
			f->exposure_us = md.get(controls::ExposureTime).value_or(-1);
			f->analogue_gain = md.get(controls::AnalogueGain).value_or(-1.0f);
			auto cg = md.get(controls::ColourGains);
			f->colour_gain_r = cg ? (*cg)[0] : -1.0f;
			f->colour_gain_b = cg ? (*cg)[1] : -1.0f;
			f->colour_temp_k = md.get(controls::ColourTemperature).value_or(-1);
			f->lux = md.get(controls::Lux).value_or(-1.0f);
			return 1;
		}
		if (now_ns() >= deadline)
			return 0;
	}
}

extern "C" void cam_stop(struct cam *c)
{
	if (!c->started)
		return;
	c->started = false;
	/* Blocks until every queued Request has come back (as Cancelled). */
	c->camera->stop();
	c->camera->requestCompleted.disconnect(c);
}

extern "C" void cam_close(struct cam *c)
{
	if (!c)
		return;
	cam_stop(c);
	c->requests.clear();
	for (auto &entry : c->maps)              /* munmap before the buffers are freed */
		for (size_t i = 0; i < entry.second.addrs.size(); i++)
			munmap(entry.second.addrs[i], entry.second.lens[i]);
	c->maps.clear();
	c->allocator.reset();                    /* frees the dmabufs */
	if (c->acquired)
		c->camera->release();
	c->camera.reset();
	c->config.reset();
	if (c->cm)
		c->cm->stop();
	delete c;                                /* runs the destructors of all members */
}
