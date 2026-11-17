// Face Censor filter for OBS: moshcap's engine/fx/censor.py (EYE BAR, FACE PIXEL, PIXELATE, NEWS BLUR) for a plain webcam.
// Faces (with eyes) come from the shared tracker (facetrack.c: YuNet on a worker thread, so the render thread never waits),
// held for a moment when the finder misses one and smoothed with the engine's speed-aware STEADY; the GPU draws the cover.

#include <obs-module.h>
#include <util/platform.h>
#include <graphics/vec2.h>
#include <graphics/vec4.h>
#include <math.h>
#include <string.h>

#include "censor-effect.h" // CENSOR_EFFECT: src/censor.effect as a string, made by build.sh
#include "facetrack.h"

#define CENSOR_VERSION "1.4.0-beta"

#define MAX_FACES FT_MAX

struct censor {
	obs_source_t *ctx;
	gs_effect_t *effect;
	gs_texrender_t *cur, *small, *bl[2];
	struct facetrack *ft;
	uint32_t w, h;
	bool rendered, rendered_log, was_cover;
	float dt, log_t;

	int mode, style, shape, levels;
	float size, length, thick, smooth, roundc, lift, block, feather, soften, strong, hold;
	uint32_t color;
	bool small_faces, cover_lost, debug;
};

static const char *censor_name(void *unused)
{
	UNUSED_PARAMETER(unused);
	return "Face Censor";
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
	f->ft = ft_create();
	blog(LOG_INFO, "[face censor] v" CENSOR_VERSION " created, finder thread %s", ft_running(f->ft) ? "running" : "FAILED");

	obs_enter_graphics();
	char *err = NULL;
	f->effect = gs_effect_create(CENSOR_EFFECT, "censor.effect", &err);
	if (!f->effect)
		blog(LOG_ERROR, "[face censor] v" CENSOR_VERSION " effect failed to compile: %s", err ? err : "?");
	bfree(err);
	f->cur = gs_texrender_create(GS_RGBA, GS_ZS_NONE);
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
	ft_destroy(f->ft);
	obs_enter_graphics();
	gs_effect_destroy(f->effect);
	gs_texrender_destroy(f->cur);
	gs_texrender_destroy(f->small);
	gs_texrender_destroy(f->bl[0]);
	gs_texrender_destroy(f->bl[1]);
	obs_leave_graphics();
	bfree(f);
}

static void censor_tick(void *data, float seconds)
{
	struct censor *f = data;
	f->rendered = false;
	f->dt = seconds > 0.0f && seconds < 0.25f ? seconds : 1.0f / 30.0f;
	ft_tick(f->ft, f->dt, f->mode == 2 ? 0.0f : f->hold);
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
	char st[512];
	ft_status(f->ft, f->log_t, st, sizeof(st));
	blog(LOG_INFO, "[face censor] v" CENSOR_VERSION " %ux%u mode=%d cover_lost=%d hold=%.2f small=%d | %s | drawn=%d cover=%d", f->w, f->h,
	     f->mode, f->cover_lost, f->hold, f->small_faces, st, nf, cover);
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
	float fc[MAX_FACES][4], roll[MAX_FACES] = {0};
	float blur_r = 0.0f;

	if (!f->rendered) {
		f->rendered = true;
		f->rendered_log = true;
		f->w = w;
		f->h = h;
		gs_blend_state_push();
		gs_enable_blending(false);
		if (!capture_source(f, w, h)) {
			gs_blend_state_pop();
			gs_enable_framebuffer_srgb(srgb);
			return;
		}
		gs_blend_state_pop();
		if (!whole)
			ft_step(f->ft, gs_texrender_get_texture(f->cur), w, h, f->small_faces, f->smooth);
	}

	// the shapes, as censor.py sizes them from the face box
	for (int i = 0; i < MAX_FACES && !whole; i++) {
		const struct ft_track *t = &f->ft->tr[i];
		if (!t->on)
			continue;
		const float x0 = t->box[0], y0 = t->box[1], x1 = t->box[2], y1 = t->box[3];
		const float bw = x1 - x0, bh = y1 - y0, cx = (x0 + x1) / 2, cy = (y0 + y1) / 2;
		float *o = fc[nf++];
		if (f->mode == 0) { // censor.py _eyebar: on the eye line, tilting with the head
			float ux = t->re[0] - t->le[0], uy = t->re[1] - t->le[1];
			const float d = fmaxf(8.0f, sqrtf(ux * ux + uy * uy));
			ux /= d, uy /= d;
			float L = fmaxf(d * f->length, bw * 0.85f * f->length / 2.6f) / 2.0f * f->size;
			float T = fmaxf(d * f->thick, bh * 0.2f * f->thick / 0.5f) / 2.0f * f->size;
			o[0] = (t->le[0] + t->re[0]) / 2 + uy * T * 2.0f * f->lift; // + lifts the bar (along the face's up)
			o[1] = (t->le[1] + t->re[1]) / 2 - ux * T * 2.0f * f->lift;
			o[2] = L;
			o[3] = T;
			roll[nf - 1] = atan2f(uy, ux);
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
		const struct ft_track *t = &f->ft->tr[i];
		tst[i] = whole || !t->on ? 0.0f : t->seen ? 1.0f : 2.0f;
		set_vec4(f, tn[i], t->box[0], t->box[1], t->box[2], t->box[3]);
	}
	set_vec4(f, "tst", tst[0], tst[1], tst[2], tst[3]);
	set_float(f, "nface", (float)nf);
	set_vec4(f, "roll", roll[0], roll[1], roll[2], roll[3]);
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
