// Face Censor filter for OBS: moshcap's engine/fx/censor.py (EYE BAR, FACE PIXEL, PIXELATE, NEWS BLUR) for a plain webcam.
// Each frame the GPU makes a small grey copy; it is read back a frame late and handed to a worker thread that finds the
// faces (facedetect.c, OpenCV's LBP face cascade, no model files), so the render thread never waits on it. Found faces
// are tracked (held for a moment when the finder misses one), smoothed with the engine's speed-aware STEADY, and drawn.

#include <obs-module.h>
#include <util/platform.h>
#include <graphics/vec2.h>
#include <graphics/vec4.h>
#include <math.h>
#include <string.h>

#include "censor-effect.h" // CENSOR_EFFECT: src/censor.effect as a string, made by build.sh
#include "facedetect.h"

#define CENSOR_VERSION "1.2.1-beta"

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
typedef SRWLOCK lock_t;
typedef CONDITION_VARIABLE cond_t;
#define lock_init(l) InitializeSRWLock(l)
#define lock(l) AcquireSRWLockExclusive(l)
#define unlock(l) ReleaseSRWLockExclusive(l)
#define cond_init(c) InitializeConditionVariable(c)
#define cond_wait(c, l) SleepConditionVariableSRW(c, l, INFINITE, 0)
#define cond_signal(c) WakeConditionVariable(c)
#else
#include <pthread.h>
typedef pthread_mutex_t lock_t;
typedef pthread_cond_t cond_t;
#define lock_init(l) pthread_mutex_init(l, NULL)
#define lock(l) pthread_mutex_lock(l)
#define unlock(l) pthread_mutex_unlock(l)
#define cond_init(c) pthread_cond_init(c, NULL)
#define cond_wait(c, l) pthread_cond_wait(c, l)
#define cond_signal(c) pthread_cond_signal(c)
#endif

#define MAX_FACES 4

// one tracked face. box = x0, y0, x1, y1 in source pixels; the follow() state is the engine's util.follow
struct track {
	bool on;
	float box[4], in[4], v[4];
	float lost; // seconds since the finder last saw it
};

struct censor {
	obs_source_t *ctx;
	gs_effect_t *effect;
	gs_texrender_t *cur, *grey, *small, *bl[2];
	gs_stagesurf_t *stage[2];
	uint32_t w, h, gw, gh;
	bool rendered, stage_ok;
	float dt;

	// the worker: `job` holds a grey frame when `pending`; results land in `res` with `seq` bumped
	lock_t lk;
	cond_t cv;
#ifdef _WIN32
	HANDLE thread;
#else
	pthread_t thread;
#endif
	bool quit, pending, running;
	uint8_t *job;
	int jw, jh;
	size_t jcap;
	struct fd_rect res[MAX_FACES];
	int nres;
	uint32_t seq, seen_seq;
	uint64_t runs, run_ns; // debug: finder runs and time spent (worker side, under lk)
	int last_raw;          // debug: faces in the last finder run

	struct track tr[MAX_FACES];

	int mode, style, shape, levels;
	float size, length, thick, smooth, roundc, lift, block, feather, soften, strong, hold;
	uint32_t color;
	bool small_faces, cover_lost, debug;
	bool was_cover, rendered_log;
	float log_t;
	bool seen_now[MAX_FACES]; // debug: track matched in the latest finder result
};

static const char *censor_name(void *unused)
{
	UNUSED_PARAMETER(unused);
	return "Face Censor";
}

// ---------------------------------------------------------------- the face-finding thread

#ifdef _WIN32
static DWORD WINAPI worker(LPVOID data)
#else
static void *worker(void *data)
#endif
{
	struct censor *f = data;
	uint8_t *buf = NULL;
	size_t cap = 0;
	lock(&f->lk);
	for (;;) {
		while (!f->pending && !f->quit)
			cond_wait(&f->cv, &f->lk);
		if (f->quit)
			break;
		const int w = f->jw, h = f->jh;
		if ((size_t)w * h > cap) {
			bfree(buf);
			cap = (size_t)w * h;
			buf = bmalloc(cap);
		}
		memcpy(buf, f->job, (size_t)w * h);
		unlock(&f->lk);

		struct fd_rect r[MAX_FACES];
		const uint64_t t0 = os_gettime_ns();
		const int n = fd_detect(buf, w, h, w, 24, r, MAX_FACES);
		const uint64_t took = os_gettime_ns() - t0;

		lock(&f->lk);
		memcpy(f->res, r, sizeof(r));
		f->nres = n;
		f->seq++;
		f->runs++;
		f->run_ns += took;
		f->last_raw = n;
		f->pending = false;
	}
	unlock(&f->lk);
	bfree(buf);
	return 0;
}

// ---------------------------------------------------------------- tracking

// util.follow: STEADY holds a still face still, a fast move passes straight through
static void follow(struct track *t, const float *nw, float s, float scale)
{
	float sq = 0.0f;
	for (int i = 0; i < 4; i++) {
		t->v[i] += ((nw[i] - t->in[i]) - t->v[i]) * 0.4f;
		sq += t->v[i] * t->v[i];
	}
	const float sp = sqrtf(sq / 2.0f) / fmaxf(scale, 1e-6f);
	const float x = fmaxf(0.0f, sp - 0.03f) / 0.05f;
	const float r = (1.0f - s) / fmaxf(s, 1e-3f) + x * x;
	const float a = r / (1.0f + r);
	for (int i = 0; i < 4; i++) {
		t->in[i] = nw[i];
		t->box[i] += (nw[i] - t->box[i]) * a;
	}
}

static void start(struct track *t, const float *b)
{
	t->on = true;
	t->lost = 0.0f;
	memcpy(t->box, b, sizeof(t->box));
	memcpy(t->in, b, sizeof(t->in));
	memset(t->v, 0, sizeof(t->v));
}

// new finder results (in grey-picture pixels, k = source pixels per grey pixel): match each to the nearest track
static void update_tracks(struct censor *f, const struct fd_rect *r, int n, float k)
{
	bool used[MAX_FACES] = {false};
	for (int i = 0; i < n; i++) {
		// the cascade's square runs brow to lip: stretch it to the whole face, chin included
		const float b[4] = {r[i].x * k, (r[i].y - 0.05f * r[i].h) * k, (r[i].x + r[i].w) * k, (r[i].y + r[i].h * 1.12f) * k};
		const float cx = (b[0] + b[2]) / 2, cy = (b[1] + b[3]) / 2, bw = b[2] - b[0];
		int best = -1;
		float bd = 1e30f;
		for (int j = 0; j < MAX_FACES; j++) {
			struct track *t = &f->tr[j];
			if (!t->on || used[j])
				continue;
			const float d = fabsf((t->box[0] + t->box[2]) / 2 - cx) + fabsf((t->box[1] + t->box[3]) / 2 - cy);
			if (d < bd) {
				bd = d;
				best = j;
			}
		}
		struct track *t = NULL;
		if (best >= 0 && bd < 2.0f * fmaxf(bw, f->tr[best].box[2] - f->tr[best].box[0])) {
			t = &f->tr[best];
			if (f->smooth > 0.0f)
				follow(t, b, f->smooth, fmaxf(bw, 20.0f));
			else
				start(t, b);
			t->lost = 0.0f;
			used[best] = true;
			continue;
		}
		for (int j = 0; j < MAX_FACES && !t; j++)
			if (!f->tr[j].on) // a jump (new person / camera cut) starts fresh instead of sliding over
				t = &f->tr[j];
		if (t) {
			start(t, b);
			used[t - f->tr] = true;
		}
	}
	memcpy(f->seen_now, used, sizeof(used));
}

// ---------------------------------------------------------------- settings

static void censor_update(void *data, obs_data_t *s)
{
	struct censor *f = data;
	f->mode = (int)obs_data_get_int(s, "mode");
	f->size = (float)obs_data_get_double(s, "size");
	f->smooth = (float)obs_data_get_double(s, "smooth");
	f->hold = (float)obs_data_get_double(s, "hold");
	f->length = (float)obs_data_get_double(s, "length");
	f->thick = (float)obs_data_get_double(s, "thick");
	f->color = (uint32_t)obs_data_get_int(s, "color");
	f->style = (int)obs_data_get_int(s, "style");
	f->roundc = (float)obs_data_get_double(s, "round");
	f->lift = (float)obs_data_get_double(s, "lift");
	f->block = (float)obs_data_get_int(s, "block");
	f->shape = (int)obs_data_get_int(s, "shape");
	f->levels = (int)obs_data_get_int(s, "levels");
	f->feather = (float)obs_data_get_double(s, "feather");
	f->soften = (float)obs_data_get_double(s, "soften");
	f->strong = (float)obs_data_get_double(s, "strong");
	f->small_faces = obs_data_get_bool(s, "small_faces");
	f->cover_lost = obs_data_get_bool(s, "cover_lost");
	f->debug = obs_data_get_bool(s, "debug");
}

// the defaults are censor.py's (mode 0 = the old EYE BAR, its most-used look)
static void censor_defaults(obs_data_t *s)
{
	obs_data_set_default_int(s, "mode", 0);
	obs_data_set_default_double(s, "size", 1.0);
	obs_data_set_default_double(s, "smooth", 0.6);
	obs_data_set_default_double(s, "hold", 0.5);
	obs_data_set_default_double(s, "length", 2.6);
	obs_data_set_default_double(s, "thick", 0.5);
	obs_data_set_default_int(s, "color", 0xFF000000);
	obs_data_set_default_int(s, "style", 0);
	obs_data_set_default_double(s, "round", 0.0);
	obs_data_set_default_double(s, "lift", 0.0);
	obs_data_set_default_int(s, "block", 16);
	obs_data_set_default_int(s, "shape", 0);
	obs_data_set_default_int(s, "levels", 0);
	obs_data_set_default_double(s, "feather", 0.15);
	obs_data_set_default_double(s, "soften", 0.6);
	obs_data_set_default_double(s, "strong", 0.75);
	obs_data_set_default_bool(s, "small_faces", false);
	obs_data_set_default_bool(s, "cover_lost", false);
	obs_data_set_default_bool(s, "debug", false);
}

static void vis(obs_properties_t *p, const char *name, bool on)
{
	obs_property_set_visible(obs_properties_get(p, name), on);
}

// show only the sliders the chosen mode uses (censor.py's "when")
static bool mode_changed(obs_properties_t *p, obs_property_t *unused, obs_data_t *s)
{
	UNUSED_PARAMETER(unused);
	const int m = (int)obs_data_get_int(s, "mode");
	const bool bar = m == 0, fp = m == 1, pix = m == 2, news = m == 3;
	vis(p, "size", !pix);
	vis(p, "smooth", !pix);
	vis(p, "hold", !pix);
	vis(p, "small_faces", !pix);
	vis(p, "cover_lost", !pix);
	vis(p, "length", bar);
	vis(p, "thick", bar);
	vis(p, "color", bar);
	vis(p, "style", bar);
	vis(p, "round", bar);
	vis(p, "lift", bar);
	vis(p, "block", fp || pix);
	vis(p, "shape", fp);
	vis(p, "levels", fp);
	vis(p, "feather", fp);
	vis(p, "soften", news);
	vis(p, "strong", news);
	return true;
}

static obs_properties_t *censor_properties(void *data)
{
	UNUSED_PARAMETER(data);
	obs_properties_t *p = obs_properties_create();
	obs_property_t *m = obs_properties_add_list(p, "mode", "Mode", OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_INT);
	obs_property_list_add_int(m, "Eye bar", 0);
	obs_property_list_add_int(m, "Face pixel", 1);
	obs_property_list_add_int(m, "Pixelate (whole picture)", 2);
	obs_property_list_add_int(m, "News blur", 3);
	obs_property_set_modified_callback(m, mode_changed);
	obs_properties_add_float_slider(p, "size", "Size", 0.5, 5.0, 0.05);
	obs_properties_add_float_slider(p, "length", "Bar length", 1.5, 4.0, 0.05);
	obs_properties_add_float_slider(p, "thick", "Bar thickness", 0.2, 1.2, 0.01);
	obs_property_t *st = obs_properties_add_list(p, "style", "Bar fill", OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_INT);
	obs_property_list_add_int(st, "Solid colour", 0);
	obs_property_list_add_int(st, "Pixels", 1);
	obs_property_list_add_int(st, "Blur", 2);
	obs_properties_add_color(p, "color", "Bar colour");
	obs_properties_add_float_slider(p, "round", "Round corners", 0.0, 1.0, 0.01);
	obs_properties_add_float_slider(p, "lift", "Up / down", -1.0, 1.0, 0.01);
	obs_properties_add_int_slider(p, "block", "Block size (px)", 2, 64, 1);
	obs_property_t *sh = obs_properties_add_list(p, "shape", "Shape", OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_INT);
	obs_property_list_add_int(sh, "Oval", 0);
	obs_property_list_add_int(sh, "Box", 1);
	obs_properties_add_int_slider(p, "levels", "Colours (0 = all)", 0, 8, 1);
	obs_properties_add_float_slider(p, "feather", "Edge softness", 0.0, 1.0, 0.01);
	obs_properties_add_float_slider(p, "soften", "Feather", 0.0, 1.0, 0.01);
	obs_properties_add_float_slider(p, "strong", "Blur strength", 0.0, 1.0, 0.01);
	obs_properties_add_float_slider(p, "smooth", "Steady", 0.0, 0.95, 0.01);
	obs_properties_add_float_slider(p, "hold", "Keep covering after a face is lost (s)", 0.0, 5.0, 0.1);
	obs_properties_add_bool(p, "small_faces", "Find smaller / farther faces (more CPU)");
	obs_properties_add_bool(p, "cover_lost", "Blur the whole picture while no face is found");
	obs_properties_add_bool(p, "debug", "Debug: outline found faces and log the face finder (for bug reports)");
	return p;
}

// ---------------------------------------------------------------- create / destroy / tick

static void *censor_create(obs_data_t *settings, obs_source_t *source)
{
	struct censor *f = bzalloc(sizeof(*f));
	f->ctx = source;
	f->dt = 1.0f / 30.0f;
	lock_init(&f->lk);
	cond_init(&f->cv);
#ifdef _WIN32
	f->thread = CreateThread(NULL, 0, worker, f, 0, NULL);
	f->running = f->thread != NULL;
#else
	f->running = pthread_create(&f->thread, NULL, worker, f) == 0;
#endif
	if (!f->running)
		blog(LOG_ERROR, "[face censor] could not start the face-finding thread");
	blog(LOG_INFO, "[face censor] v" CENSOR_VERSION " created, finder thread %s", f->running ? "running" : "FAILED");

	obs_enter_graphics();
	char *err = NULL;
	f->effect = gs_effect_create(CENSOR_EFFECT, "censor.effect", &err);
	if (!f->effect)
		blog(LOG_ERROR, "[face censor] v" CENSOR_VERSION " effect failed to compile: %s", err ? err : "?");
	bfree(err);
	f->cur = gs_texrender_create(GS_RGBA, GS_ZS_NONE);
	f->grey = gs_texrender_create(GS_RGBA, GS_ZS_NONE);
	f->small = gs_texrender_create(GS_RGBA, GS_ZS_NONE);
	f->bl[0] = gs_texrender_create(GS_RGBA, GS_ZS_NONE);
	f->bl[1] = gs_texrender_create(GS_RGBA, GS_ZS_NONE);
	obs_leave_graphics();

	censor_update(f, settings);
	return f;
}

static void censor_destroy(void *data)
{
	struct censor *f = data;
	if (f->running) {
		lock(&f->lk);
		f->quit = true;
		cond_signal(&f->cv);
		unlock(&f->lk);
#ifdef _WIN32
		WaitForSingleObject(f->thread, INFINITE);
		CloseHandle(f->thread);
#else
		pthread_join(f->thread, NULL);
#endif
	}
	obs_enter_graphics();
	gs_effect_destroy(f->effect);
	gs_texrender_destroy(f->cur);
	gs_texrender_destroy(f->grey);
	gs_texrender_destroy(f->small);
	gs_texrender_destroy(f->bl[0]);
	gs_texrender_destroy(f->bl[1]);
	gs_stagesurface_destroy(f->stage[0]);
	gs_stagesurface_destroy(f->stage[1]);
	obs_leave_graphics();
	bfree(f->job);
	bfree(f);
}

static void censor_tick(void *data, float seconds)
{
	struct censor *f = data;
	f->rendered = false;
	f->dt = seconds > 0.0f && seconds < 0.25f ? seconds : 1.0f / 30.0f;
	for (int i = 0; i < MAX_FACES; i++)
		if (f->tr[i].on && (f->tr[i].lost += f->dt) > f->hold + 0.15f) // + the finder's own gap between looks
			f->tr[i].on = false;
}

// ---------------------------------------------------------------- render

static void set_tex(struct censor *f, const char *name, gs_texrender_t *tr)
{
	gs_effect_set_texture(gs_effect_get_param_by_name(f->effect, name), gs_texrender_get_texture(tr));
}

static void set_vec2(struct censor *f, const char *name, float x, float y)
{
	struct vec2 v;
	vec2_set(&v, x, y);
	gs_effect_set_vec2(gs_effect_get_param_by_name(f->effect, name), &v);
}

static void set_vec4(struct censor *f, const char *name, float x, float y, float z, float w)
{
	struct vec4 v;
	vec4_set(&v, x, y, z, w);
	gs_effect_set_vec4(gs_effect_get_param_by_name(f->effect, name), &v);
}

static void set_float(struct censor *f, const char *name, float v)
{
	gs_effect_set_float(gs_effect_get_param_by_name(f->effect, name), v);
}

static void pass(struct censor *f, gs_texrender_t *dst, uint32_t w, uint32_t h, const char *tech)
{
	gs_texrender_reset(dst);
	if (!gs_texrender_begin(dst, w, h))
		return;
	gs_ortho(0.0f, (float)w, 0.0f, (float)h, -100.0f, 100.0f);
	while (gs_effect_loop(f->effect, tech))
		gs_draw_sprite(NULL, 0, w, h);
	gs_texrender_end(dst);
}

static bool capture_source(struct censor *f, uint32_t w, uint32_t h)
{
	if (!obs_source_process_filter_begin(f->ctx, GS_RGBA, OBS_NO_DIRECT_RENDERING))
		return false;
	gs_texrender_reset(f->cur);
	if (!gs_texrender_begin(f->cur, w, h))
		return false;
	struct vec4 clear;
	vec4_zero(&clear);
	gs_clear(GS_CLEAR_COLOR, &clear, 0.0f, 0);
	gs_ortho(0.0f, (float)w, 0.0f, (float)h, -100.0f, 100.0f);
	obs_source_process_filter_end(f->ctx, obs_get_base_effect(OBS_EFFECT_DEFAULT), w, h);
	gs_texrender_end(f->cur);
	return true;
}

// blur the source by about `r` pixels into bl[0]: shrink so r is 3 small texels, then a separable gaussian
static void make_blur(struct censor *f, float r)
{
	float k = fmaxf(1.0f, r / 3.0f);
	const uint32_t sw = (uint32_t)fmaxf(1.0f, (float)f->w / k), sh = (uint32_t)fmaxf(1.0f, (float)f->h / k);
	set_tex(f, "image", f->cur);
	set_vec2(f, "cell", k / (float)f->w, k / (float)f->h);
	pass(f, f->small, sw, sh, "Down");
	set_tex(f, "image", f->small);
	set_vec2(f, "texel", 1.0f / sw, 1.0f / sh);
	pass(f, f->bl[1], sw, sh, "BlurH");
	set_tex(f, "image", f->bl[1]);
	set_vec2(f, "texel", 1.0f / sw, 1.0f / sh);
	pass(f, f->bl[0], sw, sh, "BlurV");
}

// make the grey copy, read back last frame's and hand it to the worker if it is free, take any new results
static void find_faces(struct censor *f)
{
	const uint32_t gw = f->small_faces ? 480 : 320;
	const uint32_t gh = (uint32_t)fmaxf(24.0f, roundf((float)gw * f->h / f->w));
	if (gw != f->gw || gh != f->gh) {
		f->gw = gw;
		f->gh = gh;
		for (int i = 0; i < 2; i++) {
			gs_stagesurface_destroy(f->stage[i]);
			f->stage[i] = gs_stagesurface_create(gw, gh, GS_RGBA);
		}
		f->stage_ok = false;
	}
	set_tex(f, "image", f->cur);
	set_vec2(f, "cell", 1.0f / gw, 1.0f / gh);
	pass(f, f->grey, gw, gh, "Grey");

	if (f->stage[0] && f->stage[1]) {
		uint8_t *px;
		uint32_t ls;
		if (f->stage_ok && f->running && gs_stagesurface_map(f->stage[0], &px, &ls)) {
			lock(&f->lk);
			if (!f->pending) {
				if (f->jcap < (size_t)gw * gh) {
					bfree(f->job);
					f->jcap = (size_t)gw * gh;
					f->job = bmalloc(f->jcap);
				}
				for (uint32_t y = 0; y < gh; y++)
					for (uint32_t x = 0; x < gw; x++)
						f->job[y * gw + x] = px[y * ls + x * 4];
				f->jw = (int)gw;
				f->jh = (int)gh;
				f->pending = true;
				cond_signal(&f->cv);
			}
			unlock(&f->lk);
			gs_stagesurface_unmap(f->stage[0]);
		}
		gs_stage_texture(f->stage[1], gs_texrender_get_texture(f->grey));
		gs_stagesurf_t *t = f->stage[0];
		f->stage[0] = f->stage[1];
		f->stage[1] = t;
		f->stage_ok = true;
	}

	lock(&f->lk);
	if (f->seq != f->seen_seq) {
		f->seen_seq = f->seq;
		struct fd_rect r[MAX_FACES];
		const int n = f->nres;
		const float k = (float)f->w / (float)(f->jw > 0 ? f->jw : 1);
		memcpy(r, f->res, sizeof(r));
		unlock(&f->lk);
		update_tracks(f, r, n, k);
	} else {
		unlock(&f->lk);
	}
}

// debug: one line every 2 s (and whenever the whole-picture blur turns on or off) in the OBS log
static void debug_log(struct censor *f, int nf, bool cover)
{
	if (cover != f->was_cover) {
		f->was_cover = cover;
		blog(LOG_INFO, "[face censor] whole-picture blur %s", cover ? "ON (no face tracked)" : "off (face tracked)");
	}
	f->log_t += f->dt;
	if (f->log_t < 2.0f)
		return;
	lock(&f->lk);
	const uint64_t runs = f->runs, ns = f->run_ns;
	const int raw = f->last_raw, jw = f->jw, jh = f->jh;
	f->runs = f->run_ns = 0;
	unlock(&f->lk);
	char tr[MAX_FACES * 64] = "", one[64];
	for (int i = 0; i < MAX_FACES; i++) {
		const struct track *t = &f->tr[i];
		if (!t->on)
			continue;
		snprintf(one, sizeof(one), " #%d %s %.2fs at %.0f,%.0f %.0fx%.0f", i, f->seen_now[i] ? "seen" : "held", t->lost, t->box[0],
			 t->box[1], t->box[2] - t->box[0], t->box[3] - t->box[1]);
		strncat(tr, one, sizeof(tr) - strlen(tr) - 1);
	}
	blog(LOG_INFO,
	     "[face censor] v" CENSOR_VERSION " %ux%u mode=%d cover_lost=%d hold=%.2f small=%d | finder %dx%d thread=%s %.1f looks/s %.1f ms/look, "
	     "last look found %d | tracks:%s | drawn=%d cover=%d",
	     f->w, f->h, f->mode, f->cover_lost, f->hold, f->small_faces, jw, jh, f->running ? "ok" : "FAILED", runs / f->log_t,
	     runs ? ns / 1e6 / runs : 0.0, raw, tr[0] ? tr : " none", nf, cover);
	f->log_t = 0.0f;
}

static void censor_render(void *data, gs_effect_t *unused)
{
	UNUSED_PARAMETER(unused);
	struct censor *f = data;
	obs_source_t *target = obs_filter_get_target(f->ctx);
	const uint32_t w = target ? obs_source_get_base_width(target) : 0;
	const uint32_t h = target ? obs_source_get_base_height(target) : 0;
	if (!f->effect || !w || !h) {
		obs_source_skip_video_filter(f->ctx);
		return;
	}
	const bool srgb = gs_framebuffer_srgb_enabled();
	gs_enable_framebuffer_srgb(false);

	const bool whole = f->mode == 2;
	int nf = 0;
	float fc[MAX_FACES][4];
	float blur_r = 0.0f;

	if (!f->rendered) {
		f->rendered = true;
		f->rendered_log = true;
		if (w != f->w || h != f->h) {
			f->w = w;
			f->h = h;
			memset(f->tr, 0, sizeof(f->tr));
		}
		gs_blend_state_push();
		gs_enable_blending(false);
		if (!capture_source(f, w, h)) {
			gs_blend_state_pop();
			gs_enable_framebuffer_srgb(srgb);
			return;
		}
		if (!whole)
			find_faces(f);
		gs_blend_state_pop();
	}

	// the shapes, as censor.py sizes them from the face box
	for (int i = 0; i < MAX_FACES && !whole; i++) {
		const struct track *t = &f->tr[i];
		if (!t->on)
			continue;
		const float x0 = t->box[0], y0 = t->box[1], x1 = t->box[2], y1 = t->box[3];
		const float bw = x1 - x0, bh = y1 - y0, cx = (x0 + x1) / 2, cy = (y0 + y1) / 2;
		float *o = fc[nf++];
		if (f->mode == 0) {
			const float L = bw * 0.85f * f->length / 2.6f / 2.0f * f->size;
			const float T = bh * 0.2f * f->thick / 0.5f / 2.0f * f->size;
			o[0] = cx;
			o[1] = y0 + 0.36f * bh - 2.0f * T * f->lift; // the eye line of the stretched box; + lifts the bar
			o[2] = L;
			o[3] = T;
			if (f->style == 2)
				blur_r = fmaxf(blur_r, fmaxf(6.0f, T * 1.6f)); // twice the engine's: at webcam sizes its blur left the eyes readable
		} else {
			const float sz = f->size * (f->mode == 3 ? 1.3f : 1.0f), mn = f->mode == 3 ? 10.0f : 8.0f;
			o[0] = cx;
			o[1] = cy;
			o[2] = fmaxf(mn, bw / 2 * sz);
			o[3] = fmaxf(mn, bh / 2 * sz * 1.05f); // the box is already face-tall: less stretch than the engine's 1.15
			if (f->mode == 3)
				blur_r = fmaxf(blur_r, fmaxf(4.0f, bw * 0.4f * (0.25f + 0.75f * f->strong)));
		}
	}
	const bool cover = !whole && nf == 0 && f->cover_lost;
	if (cover)
		blur_r = fmaxf(12.0f, (float)w / 40.0f);
	if (f->debug && f->rendered_log) {
		f->rendered_log = false;
		debug_log(f, nf, cover);
	}
	if (!whole && nf == 0 && !cover && !f->debug) {
		// nothing to cover: the plain picture
		gs_effect_t *def = obs_get_base_effect(OBS_EFFECT_DEFAULT);
		gs_effect_set_texture(gs_effect_get_param_by_name(def, "image"), gs_texrender_get_texture(f->cur));
		while (gs_effect_loop(def, "Draw"))
			gs_draw_sprite(NULL, 0, w, h);
		gs_enable_framebuffer_srgb(srgb);
		return;
	}
	if (blur_r > 0.0f) {
		gs_blend_state_push();
		gs_enable_blending(false);
		make_blur(f, blur_r);
		gs_blend_state_pop();
	}

	struct vec4 col;
	vec4_from_rgba(&col, f->color);
	set_tex(f, "image", f->cur);
	set_tex(f, "blur_tex", blur_r > 0.0f ? f->bl[0] : f->cur);
	set_vec2(f, "size", (float)w, (float)h);
	set_float(f, "mode", (float)f->mode);
	set_float(f, "style", (float)f->style);
	gs_effect_set_vec4(gs_effect_get_param_by_name(f->effect, "bar_color"), &col);
	set_float(f, "round_k", f->roundc);
	set_float(f, "block", fmaxf(2.0f, f->block));
	set_float(f, "levels", (float)f->levels);
	set_float(f, "shape", (float)f->shape);
	set_float(f, "feather", f->feather);
	set_float(f, "soften", f->soften);
	set_float(f, "cover", cover ? 1.0f : 0.0f);
	set_float(f, "dbg", f->debug ? 1.0f : 0.0f);
	static const char *tn[MAX_FACES] = {"t0", "t1", "t2", "t3"};
	float tst[MAX_FACES];
	for (int i = 0; i < MAX_FACES; i++) {
		const struct track *t = &f->tr[i];
		tst[i] = whole || !t->on ? 0.0f : f->seen_now[i] ? 1.0f : 2.0f;
		set_vec4(f, tn[i], t->box[0], t->box[1], t->box[2], t->box[3]);
	}
	set_vec4(f, "tst", tst[0], tst[1], tst[2], tst[3]);
	set_float(f, "nface", (float)nf);
	static const char *fn[MAX_FACES] = {"f0", "f1", "f2", "f3"};
	for (int i = 0; i < MAX_FACES; i++)
		set_vec4(f, fn[i], i < nf ? fc[i][0] : 0, i < nf ? fc[i][1] : 0, i < nf ? fc[i][2] : 1, i < nf ? fc[i][3] : 1);
	while (gs_effect_loop(f->effect, "Out"))
		gs_draw_sprite(NULL, 0, w, h);

	gs_enable_framebuffer_srgb(srgb);
}

struct obs_source_info censor_filter = {
	.id = "face_censor_filter",
	.type = OBS_SOURCE_TYPE_FILTER,
	.output_flags = OBS_SOURCE_VIDEO,
	.get_name = censor_name,
	.create = censor_create,
	.destroy = censor_destroy,
	.update = censor_update,
	.get_defaults = censor_defaults,
	.get_properties = censor_properties,
	.video_tick = censor_tick,
	.video_render = censor_render,
};
