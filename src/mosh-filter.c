// Datamosh filter for OBS. A port of moshcap's engine/fx/mosh.py: block-matched motion drags a
// buffer of old frames along instead of letting the picture refresh.

#include <obs-module.h>
#include <graphics/vec2.h>
#include <graphics/vec4.h>

#include "mosh-effect.h" // MOSH_EFFECT: src/mosh.effect as a string, made by build.sh

OBS_DECLARE_MODULE()

struct mosh {
	obs_source_t *ctx;
	gs_effect_t *effect;
	gs_texrender_t *cur;            // this frame of the source
	gs_texrender_t *lum[2];         // quarter-size luma: [0] now, [1] last frame
	gs_texrender_t *flow;           // eighth-size motion
	gs_texrender_t *buf[2];         // mosh buffer ping-pong: [0] latest
	uint32_t w, h;
	bool have_prev, rendered, reset;
	obs_hotkey_id hotkey;
	float t, kf_t;

	float amount, refresh, scale, blocks, steady, keyframe;
	int search;
};

static const char *mosh_name(void *unused)
{
	UNUSED_PARAMETER(unused);
	return "Datamosh";
}

static void mosh_update(void *data, obs_data_t *s)
{
	struct mosh *f = data;
	f->amount = (float)obs_data_get_double(s, "amount");
	f->refresh = (float)obs_data_get_double(s, "refresh");
	f->scale = (float)obs_data_get_double(s, "scale");
	f->blocks = (float)obs_data_get_int(s, "blocks");
	f->steady = (float)obs_data_get_double(s, "steady");
	f->keyframe = (float)obs_data_get_double(s, "keyframe");
	f->search = (int)obs_data_get_int(s, "search");
}

static void mosh_defaults(obs_data_t *s)
{
	obs_data_set_default_double(s, "amount", 0.9);
	obs_data_set_default_double(s, "refresh", 0.02);
	obs_data_set_default_double(s, "scale", 1.0);
	obs_data_set_default_int(s, "blocks", 0);
	obs_data_set_default_double(s, "steady", 0.3);
	obs_data_set_default_double(s, "keyframe", 0.0);
	obs_data_set_default_int(s, "search", 6);
}

static bool mosh_reset_clicked(obs_properties_t *props, obs_property_t *p, void *data)
{
	UNUSED_PARAMETER(props);
	UNUSED_PARAMETER(p);
	((struct mosh *)data)->reset = true;
	return false;
}

static void mosh_reset_hotkey(void *data, obs_hotkey_id id, obs_hotkey_t *hk, bool pressed)
{
	UNUSED_PARAMETER(id);
	UNUSED_PARAMETER(hk);
	if (pressed)
		((struct mosh *)data)->reset = true;
}

static obs_properties_t *mosh_properties(void *data)
{
	UNUSED_PARAMETER(data);
	obs_properties_t *p = obs_properties_create();
	obs_properties_add_float_slider(p, "amount", "Amount", 0.0, 1.0, 0.01);
	obs_properties_add_float_slider(p, "refresh", "Refresh (how fast it heals)", 0.0, 0.3, 0.005);
	obs_properties_add_float_slider(p, "scale", "Flow scale", 0.0, 3.0, 0.05);
	obs_properties_add_int_slider(p, "blocks", "Block px (0 = smooth)", 0, 32, 4);
	obs_properties_add_float_slider(p, "steady", "Steady flow", 0.0, 1.0, 0.01);
	obs_properties_add_float_slider(p, "keyframe", "Auto reset every N s (0 = off)", 0.0, 20.0, 0.5);
	obs_properties_add_int_slider(p, "search", "Motion search range", 1, 12, 1);
	obs_properties_add_button(p, "reset", "Clean frame (reset)", mosh_reset_clicked);
	return p;
}

static void mosh_free_textures(struct mosh *f)
{
	gs_texrender_destroy(f->cur);
	gs_texrender_destroy(f->lum[0]);
	gs_texrender_destroy(f->lum[1]);
	gs_texrender_destroy(f->flow);
	gs_texrender_destroy(f->buf[0]);
	gs_texrender_destroy(f->buf[1]);
}

static void *mosh_create(obs_data_t *settings, obs_source_t *source)
{
	struct mosh *f = bzalloc(sizeof(*f));
	f->ctx = source;
	f->reset = true;

	obs_enter_graphics();
	char *err = NULL;
	f->effect = gs_effect_create(MOSH_EFFECT, "mosh.effect", &err);
	if (!f->effect)
		blog(LOG_ERROR, "[datamosh] effect failed to compile: %s", err ? err : "?");
	bfree(err);
	f->cur = gs_texrender_create(GS_RGBA, GS_ZS_NONE);
	f->lum[0] = gs_texrender_create(GS_R16F, GS_ZS_NONE);
	f->lum[1] = gs_texrender_create(GS_R16F, GS_ZS_NONE);
	f->flow = gs_texrender_create(GS_RG32F, GS_ZS_NONE);
	f->buf[0] = gs_texrender_create(GS_RGBA, GS_ZS_NONE);
	f->buf[1] = gs_texrender_create(GS_RGBA, GS_ZS_NONE);
	obs_leave_graphics();

	f->hotkey = obs_hotkey_register_source(source, "datamosh.reset", "Datamosh: clean frame", mosh_reset_hotkey, f);
	mosh_update(f, settings);
	return f;
}

static void mosh_destroy(void *data)
{
	struct mosh *f = data;
	obs_hotkey_unregister(f->hotkey);
	obs_enter_graphics();
	gs_effect_destroy(f->effect);
	mosh_free_textures(f);
	obs_leave_graphics();
	bfree(f);
}

static void mosh_tick(void *data, float seconds)
{
	struct mosh *f = data;
	f->rendered = false;
	f->t += seconds;
	if (f->keyframe > 0.0f && f->t - f->kf_t >= f->keyframe)
		f->reset = true;
}

static void set_tex(struct mosh *f, const char *name, gs_texrender_t *tr)
{
	gs_effect_set_texture(gs_effect_get_param_by_name(f->effect, name), gs_texrender_get_texture(tr));
}

static void set_vec2(struct mosh *f, const char *name, float x, float y)
{
	struct vec2 v;
	vec2_set(&v, x, y);
	gs_effect_set_vec2(gs_effect_get_param_by_name(f->effect, name), &v);
}

static void set_float(struct mosh *f, const char *name, float v)
{
	gs_effect_set_float(gs_effect_get_param_by_name(f->effect, name), v);
}

// one full-screen pass of technique tech into dst at w x h (params already set)
static void pass(struct mosh *f, gs_texrender_t *dst, uint32_t w, uint32_t h, const char *tech)
{
	gs_texrender_reset(dst);
	if (!gs_texrender_begin(dst, w, h))
		return;
	gs_ortho(0.0f, (float)w, 0.0f, (float)h, -100.0f, 100.0f);
	while (gs_effect_loop(f->effect, tech))
		gs_draw_sprite(NULL, 0, w, h);
	gs_texrender_end(dst);
}

static void swap(gs_texrender_t **a, gs_texrender_t **b)
{
	gs_texrender_t *t = *a;
	*a = *b;
	*b = t;
}

static bool capture_source(struct mosh *f, uint32_t w, uint32_t h)
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

// advance the mosh one frame: capture, luma, motion, drag the buffer
static void mosh_step(struct mosh *f, uint32_t w, uint32_t h)
{
	if (w != f->w || h != f->h) {
		f->w = w;
		f->h = h;
		f->reset = true;
	}
	if (!capture_source(f, w, h))
		return;

	uint32_t lw = (w + 3) / 4, lh = (h + 3) / 4;
	uint32_t fw = (w + 7) / 8, fh = (h + 7) / 8;

	swap(&f->lum[0], &f->lum[1]);
	set_tex(f, "image", f->cur);
	set_vec2(f, "texel", 1.0f / w, 1.0f / h);
	pass(f, f->lum[0], lw, lh, "Luma");

	if (f->reset) {
		// clean frame: the buffer becomes the picture again
		f->reset = false;
		f->kf_t = f->t;
		f->have_prev = true;
		set_tex(f, "image", f->cur);
		set_tex(f, "prev_tex", f->cur);
		set_float(f, "amount", 0.0f);
		pass(f, f->buf[0], w, h, "Out");
		return;
	}

	swap(&f->buf[0], &f->buf[1]);
	bool moving = f->have_prev && f->scale > 0.0f;
	if (moving) {
		set_tex(f, "image", f->lum[0]);
		set_tex(f, "prev_tex", f->lum[1]);
		set_vec2(f, "texel", 1.0f / lw, 1.0f / lh);
		gs_effect_set_int(gs_effect_get_param_by_name(f->effect, "search"), f->search);
		pass(f, f->flow, fw, fh, "Flow");
	}
	set_tex(f, "image", f->cur);
	set_tex(f, "prev_tex", f->buf[1]);
	set_tex(f, "flow_tex", f->flow);
	set_vec2(f, "texel", 1.0f / fw, 1.0f / fh);
	set_vec2(f, "size", (float)w, (float)h);
	set_float(f, "scale", moving ? f->scale : 0.0f);
	set_float(f, "steady", f->steady);
	set_float(f, "blocks", f->blocks);
	set_float(f, "refresh", f->refresh);
	pass(f, f->buf[0], w, h, "Mosh");
	f->have_prev = true;
}

static void mosh_render(void *data, gs_effect_t *unused)
{
	UNUSED_PARAMETER(unused);
	struct mosh *f = data;
	obs_source_t *target = obs_filter_get_target(f->ctx);
	uint32_t w = target ? obs_source_get_base_width(target) : 0;
	uint32_t h = target ? obs_source_get_base_height(target) : 0;
	if (!f->effect || !w || !h) {
		obs_source_skip_video_filter(f->ctx);
		return;
	}

	const bool srgb = gs_framebuffer_srgb_enabled();
	gs_enable_framebuffer_srgb(false);
	gs_blend_state_push();
	gs_blend_function(GS_BLEND_ONE, GS_BLEND_ZERO);

	// render can run more than once a frame (preview + projectors): only step once
	if (!f->rendered) {
		f->rendered = true;
		mosh_step(f, w, h);
	}
	gs_blend_state_pop();

	set_tex(f, "image", f->cur);
	set_tex(f, "prev_tex", f->buf[0]);
	set_float(f, "amount", f->amount);
	while (gs_effect_loop(f->effect, "Out"))
		gs_draw_sprite(NULL, 0, w, h);

	gs_enable_framebuffer_srgb(srgb);
}

static struct obs_source_info mosh_filter = {
	.id = "datamosh_filter",
	.type = OBS_SOURCE_TYPE_FILTER,
	.output_flags = OBS_SOURCE_VIDEO,
	.get_name = mosh_name,
	.create = mosh_create,
	.destroy = mosh_destroy,
	.update = mosh_update,
	.get_defaults = mosh_defaults,
	.get_properties = mosh_properties,
	.video_tick = mosh_tick,
	.video_render = mosh_render,
};

bool obs_module_load(void)
{
	obs_register_source(&mosh_filter);
	return true;
}
