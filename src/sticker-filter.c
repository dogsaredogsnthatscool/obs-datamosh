// Face Sticker filter for OBS: a sticker pinned to every face the shared face tracker follows (facetrack.c). Built-in
// Smiley, Sunglasses and Heart eyes are drawn by the shader; "Your own image" loads any PNG / JPG / GIF (GIFs animate).

#include <obs-module.h>
#include <graphics/graphics.h>
#include <graphics/vec2.h>
#include <graphics/vec4.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

#include "sticker-effect.h" // STICKER_EFFECT: src/sticker.effect as a string, made by build.sh
#include "facetrack.h"

#ifdef _WIN32
#include <windows.h>
#else
#include <dlfcn.h>
#endif

/* OBS 33 renamed gs_image_file_* to gs_image_file_ex_* (and dropped the old
 * names), so importing either set by name stops the DLL loading on the other
 * OBS. Look them up at run time instead. Both structs start with the same
 * fields (texture, format, cx, cy, is_animated_gif, frame_updated, loaded);
 * the rest is opaque and given plenty of room. */
struct img_file {
	gs_texture_t *texture;
	int format;
	uint32_t cx, cy;
	bool is_animated_gif, frame_updated, loaded;
	uint8_t rest[1024];
};
static struct {
	bool tried, ex;
	void (*init_ex)(void *, const char *, int);
	void (*init)(void *, const char *);
	void (*free)(void *);
	void (*init_texture)(void *);
	bool (*tick)(void *, uint64_t);
	void (*update_texture)(void *);
} img_api;

static void *obs_sym(const char *name)
{
#ifdef _WIN32
	HMODULE m = GetModuleHandleA("obs.dll");
	return m ? (void *)GetProcAddress(m, name) : NULL;
#else
	return dlsym(RTLD_DEFAULT, name);
#endif
}

static bool img_api_ok(void)
{
	if (!img_api.tried) {
		img_api.tried = true;
		img_api.init_ex = obs_sym("gs_image_file_ex_init");
		img_api.ex = img_api.init_ex != NULL;
		const char *pre = img_api.ex ? "gs_image_file_ex_" : "gs_image_file_";
		char n[64];
#define IMG_SYM(f) (snprintf(n, sizeof n, "%s%s", pre, #f), img_api.f = obs_sym(n))
		if (!img_api.ex)
			IMG_SYM(init);
		IMG_SYM(free);
		IMG_SYM(init_texture);
		IMG_SYM(tick);
		IMG_SYM(update_texture);
#undef IMG_SYM
		blog(LOG_INFO, "[face sticker] image loader: %s", img_api.ex ? "gs_image_file_ex" : "gs_image_file");
	}
	return (img_api.init_ex || img_api.init) && img_api.free && img_api.init_texture && img_api.tick &&
	       img_api.update_texture;
}

static void img_free(struct img_file *im)
{
	if (img_api_ok())
		img_api.free(im);
	memset(im, 0, sizeof(*im));
}

static void img_init(struct img_file *im, const char *path)
{
	memset(im, 0, sizeof(*im));
	if (!img_api_ok())
		return;
	if (img_api.ex)
		img_api.init_ex(im, path, 0 /* GS_IMAGE_ALPHA_STRAIGHT, what the old init used */);
	else
		img_api.init(im, path);
}

#define STICKER_VERSION "1.4.0-beta"

enum { K_SMILEY, K_GLASSES, K_HEARTS, K_IMAGE };

struct sticker {
	obs_source_t *ctx;
	gs_effect_t *effect;
	gs_texrender_t *cur;
	struct facetrack *ft;
	struct img_file img;
	bool img_ok;
	char *path;
	bool rendered, rendered_log;
	float dt, log_t;

	int kind;
	float size, ox, oy, opacity, smooth, hold;
	bool small_faces, debug;
};

static const char *sticker_name(void *unused)
{
	UNUSED_PARAMETER(unused);
	return "Face Sticker";
}

static void load_image(struct sticker *f, const char *path)
{
	if (f->path && path && !strcmp(f->path, path))
		return;
	bfree(f->path);
	f->path = path && *path ? bstrdup(path) : NULL;
	obs_enter_graphics();
	img_free(&f->img);
	obs_leave_graphics();
	f->img_ok = false;
	if (!f->path)
		return;
	img_init(&f->img, f->path);
	obs_enter_graphics();
	if (img_api_ok())
		img_api.init_texture(&f->img);
	obs_leave_graphics();
	f->img_ok = f->img.loaded && f->img.texture;
	if (!f->img_ok)
		blog(LOG_WARNING, "[face sticker] could not load image '%s'", f->path);
}

static void sticker_update(void *data, obs_data_t *s)
{
	struct sticker *f = data;
	f->kind = (int)obs_data_get_int(s, "kind");
	f->size = (float)obs_data_get_double(s, "size");
	f->ox = (float)obs_data_get_double(s, "ox");
	f->oy = (float)obs_data_get_double(s, "oy");
	f->opacity = (float)obs_data_get_double(s, "opacity");
	f->smooth = (float)obs_data_get_double(s, "smooth");
	f->hold = (float)obs_data_get_double(s, "hold");
	f->small_faces = obs_data_get_bool(s, "small_faces");
	f->debug = obs_data_get_bool(s, "debug");
	load_image(f, f->kind == K_IMAGE ? obs_data_get_string(s, "image") : NULL);
}

static void sticker_defaults(obs_data_t *s)
{
	obs_data_set_default_int(s, "kind", K_SMILEY);
	obs_data_set_default_double(s, "size", 1.0);
	obs_data_set_default_double(s, "ox", 0.0);
	obs_data_set_default_double(s, "oy", 0.0);
	obs_data_set_default_double(s, "opacity", 1.0);
	obs_data_set_default_double(s, "smooth", 0.6);
	obs_data_set_default_double(s, "hold", 0.5);
	obs_data_set_default_bool(s, "small_faces", false);
	obs_data_set_default_bool(s, "debug", false);
}

static bool kind_changed(obs_properties_t *p, obs_property_t *unused, obs_data_t *s)
{
	UNUSED_PARAMETER(unused);
	obs_property_set_visible(obs_properties_get(p, "image"), obs_data_get_int(s, "kind") == K_IMAGE);
	return true;
}

static obs_properties_t *sticker_properties(void *data)
{
	UNUSED_PARAMETER(data);
	obs_properties_t *p = obs_properties_create();
	obs_property_t *k = obs_properties_add_list(p, "kind", "Sticker", OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_INT);
	obs_property_list_add_int(k, "Smiley", K_SMILEY);
	obs_property_list_add_int(k, "Sunglasses", K_GLASSES);
	obs_property_list_add_int(k, "Heart eyes", K_HEARTS);
	obs_property_list_add_int(k, "Your own image", K_IMAGE);
	obs_property_set_modified_callback(k, kind_changed);
	obs_properties_add_path(p, "image", "Image (PNG with transparency works best)", OBS_PATH_FILE,
				"Images (*.png *.jpg *.jpeg *.gif *.webp *.bmp);;All files (*.*)", NULL);
	obs_properties_add_float_slider(p, "size", "Size", 0.3, 4.0, 0.05);
	obs_properties_add_float_slider(p, "ox", "Move left / right", -2.0, 2.0, 0.01);
	obs_properties_add_float_slider(p, "oy", "Move up / down", -2.0, 2.0, 0.01);
	obs_properties_add_float_slider(p, "opacity", "Opacity", 0.0, 1.0, 0.01);
	obs_properties_add_float_slider(p, "smooth", "Steady", 0.0, 0.95, 0.01);
	obs_properties_add_float_slider(p, "hold", "Keep showing after a face is lost (s)", 0.0, 5.0, 0.1);
	obs_properties_add_bool(p, "small_faces", "Find smaller / farther faces (more CPU)");
	obs_properties_add_bool(p, "debug", "Debug: log the face finder (for bug reports)");
	return p;
}

static void *sticker_create(obs_data_t *settings, obs_source_t *source)
{
	struct sticker *f = bzalloc(sizeof(*f));
	f->ctx = source;
	f->dt = 1.0f / 30.0f;
	f->ft = ft_create();
	blog(LOG_INFO, "[face sticker] v" STICKER_VERSION " created, finder thread %s", ft_running(f->ft) ? "running" : "FAILED");
	obs_enter_graphics();
	char *err = NULL;
	f->effect = gs_effect_create(STICKER_EFFECT, "sticker.effect", &err);
	if (!f->effect)
		blog(LOG_ERROR, "[face sticker] effect failed to compile: %s", err ? err : "?");
	bfree(err);
	f->cur = gs_texrender_create(GS_RGBA, GS_ZS_NONE);
	obs_leave_graphics();
	sticker_update(f, settings);
	return f;
}

static void sticker_destroy(void *data)
{
	struct sticker *f = data;
	ft_destroy(f->ft);
	obs_enter_graphics();
	gs_effect_destroy(f->effect);
	gs_texrender_destroy(f->cur);
	img_free(&f->img);
	obs_leave_graphics();
	bfree(f->path);
	bfree(f);
}

static void sticker_tick(void *data, float seconds)
{
	struct sticker *f = data;
	f->rendered = false;
	f->dt = seconds > 0.0f && seconds < 0.25f ? seconds : 1.0f / 30.0f;
	ft_tick(f->ft, f->dt, f->hold);
	if (f->img_ok && img_api.tick(&f->img, (uint64_t)(seconds * 1e9))) { // an animated GIF moved on a frame
		obs_enter_graphics();
		img_api.update_texture(&f->img);
		obs_leave_graphics();
	}
}

static bool capture_source(struct sticker *f, uint32_t w, uint32_t h)
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

static void set_vec4(struct sticker *f, const char *name, float x, float y, float z, float w)
{
	struct vec4 v;
	vec4_set(&v, x, y, z, w);
	gs_effect_set_vec4(gs_effect_get_param_by_name(f->effect, name), &v);
}

static void sticker_render(void *data, gs_effect_t *unused)
{
	UNUSED_PARAMETER(unused);
	struct sticker *f = data;
	obs_source_t *target = obs_filter_get_target(f->ctx);
	const uint32_t w = target ? obs_source_get_base_width(target) : 0;
	const uint32_t h = target ? obs_source_get_base_height(target) : 0;
	if (!f->effect || !w || !h) {
		obs_source_skip_video_filter(f->ctx);
		return;
	}
	const bool srgb = gs_framebuffer_srgb_enabled();
	gs_enable_framebuffer_srgb(false);

	if (!f->rendered) {
		f->rendered = true;
		f->rendered_log = true;
		gs_blend_state_push();
		gs_enable_blending(false);
		const bool ok = capture_source(f, w, h);
		gs_blend_state_pop();
		if (!ok) {
			gs_enable_framebuffer_srgb(srgb);
			return;
		}
		ft_step(f->ft, gs_texrender_get_texture(f->cur), w, h, f->small_faces, f->smooth);
	}

	// where each sticker goes, from the face box
	const bool own = f->kind == K_IMAGE;
	const float aspect = own && f->img_ok && f->img.cx ? (float)f->img.cy / (float)f->img.cx : 1.0f;
	float r[FT_MAX][4], fd[FT_MAX] = {0};
	int n = 0;
	for (int i = 0; i < FT_MAX; i++) {
		const struct ft_track *t = &f->ft->tr[i];
		if (!t->on || (own && !f->img_ok))
			continue;
		const float bw = t->box[2] - t->box[0], bh = t->box[3] - t->box[1];
		float cx = (t->box[0] + t->box[2]) / 2, cy = (t->box[1] + t->box[3]) / 2, sw, sh;
		if (f->kind == K_GLASSES || f->kind == K_HEARTS) { // on the eyes, sized from the eye distance
			const float d = fmaxf(8.0f, hypotf(t->re[0] - t->le[0], t->re[1] - t->le[1]));
			cx = (t->le[0] + t->re[0]) / 2;
			cy = (t->le[1] + t->re[1]) / 2;
			sw = fmaxf(d * (f->kind == K_GLASSES ? 2.35f : 2.0f), bw * 0.6f) * f->size;
			sh = sw / (f->kind == K_GLASSES ? 2.6f : 2.2f);
		} else {
			sw = bw * (own ? 1.3f : 1.25f) * f->size;
			sh = sw * aspect;
		}
		cx += f->ox * bw;
		cy -= f->oy * bh;
		r[n][0] = cx - sw / 2;
		r[n][1] = cy - sh / 2;
		r[n][2] = cx + sw / 2;
		r[n][3] = cy + sh / 2;
		fd[n] = fminf(1.0f, fmaxf(0.0f, (f->hold + 0.15f - t->lost) / 0.2f)); // fades out over the end of the hold
		n++;
	}

	if (f->debug && f->rendered_log) {
		f->rendered_log = false;
		if ((f->log_t += f->dt) >= 2.0f) {
			char st[512];
			ft_status(f->ft, f->log_t, st, sizeof(st));
			blog(LOG_INFO, "[face sticker] v" STICKER_VERSION " %ux%u kind=%d image=%s | %s | drawn=%d", w, h, f->kind,
			     own ? (f->img_ok ? "ok" : "NOT LOADED") : "-", st, n);
			f->log_t = 0.0f;
		}
	}

	gs_effect_set_texture(gs_effect_get_param_by_name(f->effect, "image"), gs_texrender_get_texture(f->cur));
	gs_effect_set_texture(gs_effect_get_param_by_name(f->effect, "sticker"),
			      f->img_ok ? f->img.texture : gs_texrender_get_texture(f->cur));
	struct vec2 sz;
	vec2_set(&sz, (float)w, (float)h);
	gs_effect_set_vec2(gs_effect_get_param_by_name(f->effect, "size"), &sz);
	gs_effect_set_float(gs_effect_get_param_by_name(f->effect, "kind"), (float)f->kind);
	gs_effect_set_float(gs_effect_get_param_by_name(f->effect, "opacity"), f->opacity);
	gs_effect_set_float(gs_effect_get_param_by_name(f->effect, "nface"), (float)n);
	static const char *sn[FT_MAX] = {"s0", "s1", "s2", "s3"};
	for (int i = 0; i < FT_MAX; i++)
		set_vec4(f, sn[i], i < n ? r[i][0] : 0, i < n ? r[i][1] : 0, i < n ? r[i][2] : 1, i < n ? r[i][3] : 1);
	set_vec4(f, "fade", fd[0], fd[1], fd[2], fd[3]);
	while (gs_effect_loop(f->effect, "Out"))
		gs_draw_sprite(NULL, 0, w, h);

	gs_enable_framebuffer_srgb(srgb);
}

struct obs_source_info sticker_filter = {
	.id = "face_sticker_filter",
	.type = OBS_SOURCE_TYPE_FILTER,
	.output_flags = OBS_SOURCE_VIDEO,
	.get_name = sticker_name,
	.create = sticker_create,
	.destroy = sticker_destroy,
	.update = sticker_update,
	.get_defaults = sticker_defaults,
	.get_properties = sticker_properties,
	.video_tick = sticker_tick,
	.video_render = sticker_render,
};
