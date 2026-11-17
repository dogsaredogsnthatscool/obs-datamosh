// Laser Eyes filter for OBS: engine/fx/lasereyes.py. Eyes and nose come from the shared face tracker (facetrack.c, YuNet);
// which way the head points is solved from them with a small 3D face model (same maths as the engine's head_dir), smoothed,
// and levelled to each person's own straight-ahead. The GPU draws the beams.

#include <obs-module.h>
#include <graphics/vec2.h>
#include <graphics/vec4.h>
#include <math.h>
#include <string.h>

#include "lasers-effect.h" // LASERS_EFFECT: src/lasers.effect as a string, made by build.sh
#include "facetrack.h"

#define LASERS_VERSION "1.4.0-beta"
#define NOSE_DROP 0.58f // Q: nose tip below the eye line, in eye distances (where YuNet puts it facing the camera)
#define NOSE_OUT 0.50f  // P: nose tip in front of the eyes

struct aim {
	float in[2], v[2], out[2]; // util.follow state for (ux * fore, uy * fore)
	bool has;
	float drop;                // this person's levelled nose drop
};

struct lasers {
	obs_source_t *ctx;
	gs_effect_t *effect;
	gs_texrender_t *cur;
	struct facetrack *ft;
	struct aim aim[FT_MAX];
	bool rendered, rendered_log;
	float t, dt, log_t;

	bool follow, level, small_faces, debug;
	float angle, length, width, sens, steady, hold;
	uint32_t color;
};

static const char *lasers_name(void *unused)
{
	UNUSED_PARAMETER(unused);
	return "Laser Eyes";
}

// ---------------------------------------------------------------- aim (lasereyes.py head_dir)

static float kq(float q, float P, float Q)
{
	return P * cosf(q) - Q * sinf(q);
}

// From the eyes (picture's left, right) and nose tip: beam direction (ux, uy), how much of the facing crosses the picture
// (fore: 0 = straight at the camera), and the nose drop seen straight on (for levelling).
static void head_dir(const float *le, const float *re, const float *nose, float drop, float sens, float *ux, float *uy, float *fore,
		     float *c_out)
{
	const float ex = re[0] - le[0], ey = re[1] - le[1];
	const float span = fmaxf(1e-3f, sqrtf(ex * ex + ey * ey));
	const float u[2] = {ex / span, ey / span}, v[2] = {-u[1], u[0]};
	const float mx = (le[0] + re[0]) / 2, my = (le[1] + re[1]) / 2;
	const float nu = (nose[0] - mx) * u[0] + (nose[1] - my) * u[1];
	const float nv = (nose[0] - mx) * v[0] + (nose[1] - my) * v[1];
	// nu / span = tan(yaw) k(p), nv / span = g(p) sqrt(1 + tan^2): one rising equation in p, bisected
	const float Q = drop, P = NOSE_OUT, A = nu / span, B = nv / span;
#define F(q) ((Q * cosf(q) + P * sinf(q)) * sqrtf(1.0f + (A / kq(q, P, Q)) * (A / kq(q, P, Q))) - B)
	float lo = -0.95f, hi = atan2f(P, Q) - 0.05f, pitch;
	if (F(lo) >= 0.0f) {
		pitch = lo;
	} else if (F(hi) <= 0.0f) {
		pitch = hi;
	} else {
		for (int i = 0; i < 30; i++) {
			const float mid = (lo + hi) / 2;
			if (F(mid) < 0.0f)
				lo = mid;
			else
				hi = mid;
		}
		pitch = (lo + hi) / 2;
	}
#undef F
	float yaw = atanf(A / kq(pitch, P, Q));
	*c_out = nv * cosf(yaw) / span;
	yaw *= sens;
	pitch *= sens;
	const float fx = sinf(yaw) * cosf(pitch), fy = sinf(pitch);
	const float f = sqrtf(fx * fx + fy * fy);
	if (f < 1e-4f) {
		*ux = 0.0f, *uy = 1.0f, *fore = 0.0f;
		return;
	}
	const float dx = fx / f, dy = fy / f;
	*ux = dx * u[0] + dy * v[0];
	*uy = dx * u[1] + dy * v[1];
	*fore = fminf(1.0f, f);
}

// util.follow on two numbers (scale 1, v0 0.03, v1 0.06 as in lasereyes.py)
static void follow2(struct aim *a, const float *nw, float s)
{
	if (!a->has) {
		memcpy(a->in, nw, sizeof(a->in));
		memcpy(a->out, nw, sizeof(a->out));
		a->v[0] = a->v[1] = 0.0f;
		a->has = true;
		return;
	}
	float sq = 0.0f;
	for (int i = 0; i < 2; i++) {
		a->v[i] += ((nw[i] - a->in[i]) - a->v[i]) * 0.4f;
		sq += a->v[i] * a->v[i];
	}
	const float x = fmaxf(0.0f, sqrtf(sq) - 0.03f) / 0.06f;
	const float r = (1.0f - s) / fmaxf(s, 1e-3f) + x * x;
	const float k = r / (1.0f + r);
	for (int i = 0; i < 2; i++) {
		a->in[i] = nw[i];
		a->out[i] += (nw[i] - a->out[i]) * k;
	}
}

// ---------------------------------------------------------------- settings

static void lasers_update(void *data, obs_data_t *s)
{
	struct lasers *f = data;
	f->follow = obs_data_get_bool(s, "follow");
	f->angle = (float)obs_data_get_double(s, "angle");
	f->length = (float)obs_data_get_double(s, "length");
	f->width = (float)obs_data_get_double(s, "width");
	f->color = (uint32_t)obs_data_get_int(s, "color");
	f->sens = (float)obs_data_get_double(s, "sens");
	f->steady = (float)obs_data_get_double(s, "steady");
	f->level = obs_data_get_bool(s, "level");
	f->hold = (float)obs_data_get_double(s, "hold");
	f->small_faces = obs_data_get_bool(s, "small_faces");
	f->debug = obs_data_get_bool(s, "debug");
}

// lasereyes.py's defaults (length 900 px and width 9 px there are for a 720-line picture: here they scale with the picture)
static void lasers_defaults(obs_data_t *s)
{
	obs_data_set_default_bool(s, "follow", true);
	obs_data_set_default_double(s, "angle", 160.0);
	obs_data_set_default_double(s, "length", 1.25);
	obs_data_set_default_double(s, "width", 9.0);
	obs_data_set_default_int(s, "color", 0xFF1A1AFF); // #ff1a1a (OBS colours are 0xAABBGGRR)
	obs_data_set_default_double(s, "sens", 1.0);
	obs_data_set_default_double(s, "steady", 0.5);
	obs_data_set_default_bool(s, "level", true);
	obs_data_set_default_double(s, "hold", 0.5);
	obs_data_set_default_bool(s, "small_faces", false);
	obs_data_set_default_bool(s, "debug", false);
}

static bool follow_changed(obs_properties_t *p, obs_property_t *unused, obs_data_t *s)
{
	UNUSED_PARAMETER(unused);
	const bool on = obs_data_get_bool(s, "follow");
	obs_property_set_visible(obs_properties_get(p, "angle"), !on);
	obs_property_set_visible(obs_properties_get(p, "sens"), on);
	obs_property_set_visible(obs_properties_get(p, "steady"), on);
	obs_property_set_visible(obs_properties_get(p, "level"), on);
	return true;
}

static obs_properties_t *lasers_properties(void *data)
{
	UNUSED_PARAMETER(data);
	obs_properties_t *p = obs_properties_create();
	obs_property_t *fo = obs_properties_add_bool(p, "follow", "Aim where I look");
	obs_property_set_modified_callback(fo, follow_changed);
	obs_properties_add_float_slider(p, "angle", "Aim (degrees)", 0.0, 360.0, 1.0);
	obs_properties_add_float_slider(p, "length", "Length (x picture height)", 0.1, 3.0, 0.05);
	obs_properties_add_float_slider(p, "width", "Width (px at 720p)", 2.0, 30.0, 0.5);
	obs_properties_add_color(p, "color", "Colour");
	obs_properties_add_float_slider(p, "sens", "Aim sensitivity", 0.5, 2.0, 0.05);
	obs_properties_add_float_slider(p, "steady", "Steady", 0.0, 0.95, 0.01);
	obs_properties_add_bool(p, "level", "Auto level (learns your own straight-ahead)");
	obs_properties_add_float_slider(p, "hold", "Keep firing after a face is lost (s)", 0.0, 5.0, 0.1);
	obs_properties_add_bool(p, "small_faces", "Find smaller / farther faces (more CPU)");
	obs_properties_add_bool(p, "debug", "Debug: log the face finder and aim (for bug reports)");
	return p;
}

static void *lasers_create(obs_data_t *settings, obs_source_t *source)
{
	struct lasers *f = bzalloc(sizeof(*f));
	f->ctx = source;
	f->dt = 1.0f / 30.0f;
	for (int i = 0; i < FT_MAX; i++)
		f->aim[i].drop = NOSE_DROP;
	f->ft = ft_create();
	blog(LOG_INFO, "[laser eyes] v" LASERS_VERSION " created, finder thread %s", ft_running(f->ft) ? "running" : "FAILED");
	obs_enter_graphics();
	char *err = NULL;
	f->effect = gs_effect_create(LASERS_EFFECT, "lasers.effect", &err);
	if (!f->effect)
		blog(LOG_ERROR, "[laser eyes] effect failed to compile: %s", err ? err : "?");
	bfree(err);
	f->cur = gs_texrender_create(GS_RGBA, GS_ZS_NONE);
	obs_leave_graphics();
	lasers_update(f, settings);
	return f;
}

static void lasers_destroy(void *data)
{
	struct lasers *f = data;
	ft_destroy(f->ft);
	obs_enter_graphics();
	gs_effect_destroy(f->effect);
	gs_texrender_destroy(f->cur);
	obs_leave_graphics();
	bfree(f);
}

static void lasers_tick(void *data, float seconds)
{
	struct lasers *f = data;
	f->rendered = false;
	f->dt = seconds > 0.0f && seconds < 0.25f ? seconds : 1.0f / 30.0f;
	f->t += f->dt;
	ft_tick(f->ft, f->dt, f->hold);
	for (int i = 0; i < FT_MAX; i++)
		if (!f->ft->tr[i].on) {
			f->aim[i].has = false;
			f->aim[i].drop = NOSE_DROP;
		}
}

static bool capture_source(struct lasers *f, uint32_t w, uint32_t h)
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

static void set_vec4(struct lasers *f, const char *name, float x, float y, float z, float w)
{
	struct vec4 v;
	vec4_set(&v, x, y, z, w);
	gs_effect_set_vec4(gs_effect_get_param_by_name(f->effect, name), &v);
}

static void lasers_render(void *data, gs_effect_t *unused)
{
	UNUSED_PARAMETER(unused);
	struct lasers *f = data;
	obs_source_t *target = obs_filter_get_target(f->ctx);
	const uint32_t w = target ? obs_source_get_base_width(target) : 0;
	const uint32_t h = target ? obs_source_get_base_height(target) : 0;
	if (!f->effect || !w || !h) {
		obs_source_skip_video_filter(f->ctx);
		return;
	}
	const bool srgb = gs_framebuffer_srgb_enabled();
	gs_enable_framebuffer_srgb(false);

	bool fresh = false;
	if (!f->rendered) {
		f->rendered = true;
		f->rendered_log = true;
		fresh = true;
		gs_blend_state_push();
		gs_enable_blending(false);
		const bool ok = capture_source(f, w, h);
		gs_blend_state_pop();
		if (!ok) {
			gs_enable_framebuffer_srgb(srgb);
			return;
		}
		ft_step(f->ft, gs_texrender_get_texture(f->cur), w, h, f->small_faces, 0.3f);
	}

	float e[FT_MAX][4], a[FT_MAX][4];
	int n = 0;
	for (int i = 0; i < FT_MAX; i++) {
		const struct ft_track *t = &f->ft->tr[i];
		if (!t->on)
			continue;
		struct aim *am = &f->aim[i];
		float ux, uy, fore;
		if (f->follow) {
			if (fresh) { // once per frame: aim, level, smooth
				float c;
				head_dir(t->le, t->re, t->nose, am->drop, f->sens, &ux, &uy, &fore, &c);
				if (f->level) {
					if (fore < 0.35f && fabsf(c - am->drop) < 0.3f)
						am->drop = fminf(0.95f, fmaxf(0.35f, am->drop + (c - am->drop) * 0.03f));
				} else {
					am->drop = NOSE_DROP;
				}
				const float nw[2] = {ux * fore, uy * fore};
				follow2(am, nw, f->steady);
			}
			fore = fminf(1.0f, sqrtf(am->out[0] * am->out[0] + am->out[1] * am->out[1]));
			ux = fore > 1e-3f ? am->out[0] / fore : 0.0f;
			uy = fore > 1e-3f ? am->out[1] / fore : 1.0f;
		} else {
			const float th = f->angle * 3.14159265f / 180.0f;
			ux = cosf(th), uy = sinf(th), fore = 1.0f;
		}
		const float d = fmaxf(8.0f, sqrtf((t->re[0] - t->le[0]) * (t->re[0] - t->le[0]) + (t->re[1] - t->le[1]) * (t->re[1] - t->le[1])));
		e[n][0] = t->le[0], e[n][1] = t->le[1], e[n][2] = t->re[0], e[n][3] = t->re[1];
		a[n][0] = ux, a[n][1] = uy, a[n][2] = fore, a[n][3] = d;
		n++;
	}

	if (f->debug && f->rendered_log) {
		f->rendered_log = false;
		if ((f->log_t += f->dt) >= 2.0f) {
			char st[512];
			ft_status(f->ft, f->log_t, st, sizeof(st));
			blog(LOG_INFO, "[laser eyes] v" LASERS_VERSION " %ux%u follow=%d | %s | aim0 %.2f,%.2f fore %.2f drop %.2f", w, h, f->follow,
			     st, n ? a[0][0] : 0.0, n ? a[0][1] : 0.0, n ? a[0][2] : 0.0, f->aim[0].drop);
			f->log_t = 0.0f;
		}
	}

	struct vec4 col;
	vec4_from_rgba(&col, f->color);
	gs_effect_set_texture(gs_effect_get_param_by_name(f->effect, "image"), gs_texrender_get_texture(f->cur));
	struct vec2 sz;
	vec2_set(&sz, (float)w, (float)h);
	gs_effect_set_vec2(gs_effect_get_param_by_name(f->effect, "size"), &sz);
	gs_effect_set_vec4(gs_effect_get_param_by_name(f->effect, "color"), &col);
	const float k = (float)h / 720.0f;
	gs_effect_set_float(gs_effect_get_param_by_name(f->effect, "len"), f->length * (float)h);
	gs_effect_set_float(gs_effect_get_param_by_name(f->effect, "width"), f->width * k * (0.85f + 0.15f * sinf(f->t * 37.0f)));
	gs_effect_set_float(gs_effect_get_param_by_name(f->effect, "nface"), (float)n);
	static const char *en[FT_MAX] = {"e0", "e1", "e2", "e3"}, *an[FT_MAX] = {"a0", "a1", "a2", "a3"};
	for (int i = 0; i < FT_MAX; i++) {
		set_vec4(f, en[i], i < n ? e[i][0] : 0, i < n ? e[i][1] : 0, i < n ? e[i][2] : 0, i < n ? e[i][3] : 0);
		set_vec4(f, an[i], i < n ? a[i][0] : 0, i < n ? a[i][1] : 1, i < n ? a[i][2] : 0, i < n ? a[i][3] : 8);
	}
	while (gs_effect_loop(f->effect, "Out"))
		gs_draw_sprite(NULL, 0, w, h);
	gs_enable_framebuffer_srgb(srgb);
}

struct obs_source_info lasers_filter = {
	.id = "laser_eyes_filter",
	.type = OBS_SOURCE_TYPE_FILTER,
	.output_flags = OBS_SOURCE_VIDEO,
	.get_name = lasers_name,
	.create = lasers_create,
	.destroy = lasers_destroy,
	.update = lasers_update,
	.get_defaults = lasers_defaults,
	.get_properties = lasers_properties,
	.video_tick = lasers_tick,
	.video_render = lasers_render,
};
