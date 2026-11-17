#include "facetrack.h"
#include "facedetect.h"
#include <graphics/vec2.h>
#include <util/platform.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

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

// the grey copy the finder reads: each output texel the average of its cell (4x4 taps), brightness in red
static const char GREY_EFFECT[] = "uniform float4x4 ViewProj;\n"
				  "uniform texture2d image;\n"
				  "uniform float2 cell;\n"
				  "sampler_state lin { Filter = Linear; AddressU = Clamp; AddressV = Clamp; };\n"
				  "struct VertData { float4 pos : POSITION; float2 uv : TEXCOORD0; };\n"
				  "VertData VSDefault(VertData v_in)\n"
				  "{\n"
				  "	VertData vert_out;\n"
				  "	vert_out.pos = mul(float4(v_in.pos.xyz, 1.0), ViewProj);\n"
				  "	vert_out.uv = v_in.uv;\n"
				  "	return vert_out;\n"
				  "}\n"
				  "float4 PSGrey(VertData v_in) : TARGET\n"
				  "{\n"
				  "	float4 s = float4(0.0, 0.0, 0.0, 0.0);\n"
				  "	for (int j = 0; j < 4; j++) {\n"
				  "		for (int i = 0; i < 4; i++)\n"
				  "			s += image.SampleLevel(lin, v_in.uv + (float2(i, j) - 1.5) * 0.25 * cell, 0.0);\n"
				  "	}\n"
				  "	float y = dot(s.rgb / 16.0, float3(0.299, 0.587, 0.114));\n"
				  "	return float4(y, y, y, 1.0);\n"
				  "}\n"
				  "technique Grey { pass { vertex_shader = VSDefault(v_in); pixel_shader = PSGrey(v_in); } }\n";

struct ft_priv {
	gs_effect_t *effect;
	gs_texrender_t *grey;
	gs_stagesurf_t *stage[2]; // readback, a frame apart so the GPU never waits
	uint32_t gw, gh;
	bool stage_ok;

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
	size_t jcap;
	int jw, jh;
	struct fd_rect res[FT_MAX];
	int nres, last_raw;
	uint32_t seq, seen_seq;
	uint64_t runs, run_ns;
};

#ifdef _WIN32
static DWORD WINAPI worker(LPVOID data)
#else
static void *worker(void *data)
#endif
{
	struct ft_priv *p = data;
	uint8_t *buf = NULL;
	size_t cap = 0;
	lock(&p->lk);
	for (;;) {
		while (!p->pending && !p->quit)
			cond_wait(&p->cv, &p->lk);
		if (p->quit)
			break;
		const int w = p->jw, h = p->jh;
		if ((size_t)w * h > cap) {
			bfree(buf);
			cap = (size_t)w * h;
			buf = bmalloc(cap);
		}
		memcpy(buf, p->job, (size_t)w * h);
		unlock(&p->lk);

		struct fd_rect r[FT_MAX];
		const uint64_t t0 = os_gettime_ns();
		const int n = fd_detect(buf, w, h, w, 24, r, FT_MAX);
		const uint64_t took = os_gettime_ns() - t0;

		lock(&p->lk);
		memcpy(p->res, r, sizeof(r));
		p->nres = p->last_raw = n;
		p->seq++;
		p->runs++;
		p->run_ns += took;
		p->pending = false;
	}
	unlock(&p->lk);
	bfree(buf);
	return 0;
}

struct facetrack *ft_create(void)
{
	struct facetrack *ft = bzalloc(sizeof(*ft));
	struct ft_priv *p = ft->p = bzalloc(sizeof(*p));
	lock_init(&p->lk);
	cond_init(&p->cv);
#ifdef _WIN32
	p->thread = CreateThread(NULL, 0, worker, p, 0, NULL);
	p->running = p->thread != NULL;
#else
	p->running = pthread_create(&p->thread, NULL, worker, p) == 0;
#endif
	if (!p->running)
		blog(LOG_ERROR, "[face tracker] could not start the face-finding thread");
	obs_enter_graphics();
	char *err = NULL;
	p->effect = gs_effect_create(GREY_EFFECT, "facetrack-grey.effect", &err);
	if (!p->effect)
		blog(LOG_ERROR, "[face tracker] grey effect failed to compile: %s", err ? err : "?");
	bfree(err);
	p->grey = gs_texrender_create(GS_RGBA, GS_ZS_NONE);
	obs_leave_graphics();
	return ft;
}

void ft_destroy(struct facetrack *ft)
{
	if (!ft)
		return;
	struct ft_priv *p = ft->p;
	if (p->running) {
		lock(&p->lk);
		p->quit = true;
		cond_signal(&p->cv);
		unlock(&p->lk);
#ifdef _WIN32
		WaitForSingleObject(p->thread, INFINITE);
		CloseHandle(p->thread);
#else
		pthread_join(p->thread, NULL);
#endif
	}
	obs_enter_graphics();
	gs_effect_destroy(p->effect);
	gs_texrender_destroy(p->grey);
	gs_stagesurface_destroy(p->stage[0]);
	gs_stagesurface_destroy(p->stage[1]);
	obs_leave_graphics();
	bfree(p->job);
	bfree(p);
	bfree(ft);
}

bool ft_running(const struct facetrack *ft)
{
	return ft->p->running;
}

void ft_tick(struct facetrack *ft, float dt, float hold)
{
	for (int i = 0; i < FT_MAX; i++) {
		struct ft_track *t = &ft->tr[i];
		if (t->on && (t->lost += dt) > hold + 0.15f) // + the finder's own gap between looks
			t->on = false;
	}
}

// util.follow: STEADY holds a still face still, a fast move passes straight through
static void follow(struct ft_track *t, const float *nw, float s, float scale)
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

static void start(struct ft_track *t, const float *b)
{
	t->on = true;
	t->lost = 0.0f;
	memcpy(t->box, b, sizeof(t->box));
	memcpy(t->in, b, sizeof(t->in));
	memset(t->v, 0, sizeof(t->v));
}

// new finder results (in grey-picture pixels, k = source pixels per grey pixel): match each to the nearest track
static void update_tracks(struct facetrack *ft, const struct fd_rect *r, int n, float k, float smooth)
{
	bool used[FT_MAX] = {false};
	for (int i = 0; i < n; i++) {
		// the cascade's square runs brow to lip: stretch it to the whole face, chin included
		const float b[4] = {r[i].x * k, (r[i].y - 0.05f * r[i].h) * k, (r[i].x + r[i].w) * k, (r[i].y + r[i].h * 1.12f) * k};
		const float cx = (b[0] + b[2]) / 2, cy = (b[1] + b[3]) / 2, bw = b[2] - b[0];
		int best = -1;
		float bd = 1e30f;
		for (int j = 0; j < FT_MAX; j++) {
			struct ft_track *t = &ft->tr[j];
			if (!t->on || used[j])
				continue;
			const float d = fabsf((t->box[0] + t->box[2]) / 2 - cx) + fabsf((t->box[1] + t->box[3]) / 2 - cy);
			if (d < bd) {
				bd = d;
				best = j;
			}
		}
		if (best >= 0 && bd < 2.0f * fmaxf(bw, ft->tr[best].box[2] - ft->tr[best].box[0])) {
			struct ft_track *t = &ft->tr[best];
			if (smooth > 0.0f)
				follow(t, b, smooth, fmaxf(bw, 20.0f));
			else
				start(t, b);
			t->lost = 0.0f;
			used[best] = true;
			continue;
		}
		for (int j = 0; j < FT_MAX; j++)
			if (!ft->tr[j].on) { // a jump (new person / camera cut) starts fresh instead of sliding over
				start(&ft->tr[j], b);
				used[j] = true;
				break;
			}
	}
	for (int j = 0; j < FT_MAX; j++)
		ft->tr[j].seen = used[j];
}

void ft_step(struct facetrack *ft, gs_texture_t *src, uint32_t w, uint32_t h, bool small_faces, float smooth)
{
	struct ft_priv *p = ft->p;
	if (!p->effect || !src || !w || !h)
		return;
	if (w != ft->w || h != ft->h) {
		ft->w = w;
		ft->h = h;
		memset(ft->tr, 0, sizeof(ft->tr));
	}
	const uint32_t gw = small_faces ? 480 : 320;
	const uint32_t gh = (uint32_t)fmaxf(24.0f, roundf((float)gw * h / w));
	if (gw != p->gw || gh != p->gh) {
		p->gw = gw;
		p->gh = gh;
		for (int i = 0; i < 2; i++) {
			gs_stagesurface_destroy(p->stage[i]);
			p->stage[i] = gs_stagesurface_create(gw, gh, GS_RGBA);
		}
		p->stage_ok = false;
	}

	gs_blend_state_push();
	gs_enable_blending(false);
	gs_effect_set_texture(gs_effect_get_param_by_name(p->effect, "image"), src);
	struct vec2 c;
	vec2_set(&c, 1.0f / gw, 1.0f / gh);
	gs_effect_set_vec2(gs_effect_get_param_by_name(p->effect, "cell"), &c);
	gs_texrender_reset(p->grey);
	if (gs_texrender_begin(p->grey, gw, gh)) {
		gs_ortho(0.0f, (float)gw, 0.0f, (float)gh, -100.0f, 100.0f);
		while (gs_effect_loop(p->effect, "Grey"))
			gs_draw_sprite(NULL, 0, gw, gh);
		gs_texrender_end(p->grey);
	}
	gs_blend_state_pop();

	if (p->stage[0] && p->stage[1]) {
		uint8_t *px;
		uint32_t ls;
		if (p->stage_ok && p->running && gs_stagesurface_map(p->stage[0], &px, &ls)) {
			lock(&p->lk);
			if (!p->pending) {
				if (p->jcap < (size_t)gw * gh) {
					bfree(p->job);
					p->jcap = (size_t)gw * gh;
					p->job = bmalloc(p->jcap);
				}
				for (uint32_t y = 0; y < gh; y++)
					for (uint32_t x = 0; x < gw; x++)
						p->job[y * gw + x] = px[y * ls + x * 4];
				p->jw = (int)gw;
				p->jh = (int)gh;
				p->pending = true;
				cond_signal(&p->cv);
			}
			unlock(&p->lk);
			gs_stagesurface_unmap(p->stage[0]);
		}
		gs_stage_texture(p->stage[1], gs_texrender_get_texture(p->grey));
		gs_stagesurf_t *t = p->stage[0];
		p->stage[0] = p->stage[1];
		p->stage[1] = t;
		p->stage_ok = true;
	}

	lock(&p->lk);
	if (p->seq != p->seen_seq) {
		p->seen_seq = p->seq;
		struct fd_rect r[FT_MAX];
		const int n = p->nres;
		const float k = (float)w / (float)(p->jw > 0 ? p->jw : 1);
		memcpy(r, p->res, sizeof(r));
		unlock(&p->lk);
		update_tracks(ft, r, n, k, smooth);
	} else {
		unlock(&p->lk);
	}
}

void ft_status(struct facetrack *ft, float seconds, char *out, size_t size)
{
	struct ft_priv *p = ft->p;
	lock(&p->lk);
	const uint64_t runs = p->runs, ns = p->run_ns;
	const int raw = p->last_raw, jw = p->jw, jh = p->jh;
	p->runs = p->run_ns = 0;
	unlock(&p->lk);
	int o = snprintf(out, size, "finder %dx%d thread=%s %.1f looks/s %.1f ms/look, last look found %d | tracks:", jw, jh,
			 p->running ? "ok" : "FAILED", seconds > 0 ? runs / seconds : 0.0, runs ? ns / 1e6 / runs : 0.0, raw);
	bool any = false;
	for (int i = 0; i < FT_MAX && o > 0 && (size_t)o < size; i++) {
		const struct ft_track *t = &ft->tr[i];
		if (!t->on)
			continue;
		any = true;
		o += snprintf(out + o, size - o, " #%d %s %.2fs at %.0f,%.0f %.0fx%.0f", i, t->seen ? "seen" : "held", t->lost, t->box[0],
			      t->box[1], t->box[2] - t->box[0], t->box[3] - t->box[1]);
	}
	if (!any && o > 0 && (size_t)o < size)
		snprintf(out + o, size - o, " none");
}
