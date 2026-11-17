// Rainbow Trail filter for OBS: moshcap's TIME > RAINBOW TRAIL (engine/fx/time.py _trail). Your past silhouettes trail
// behind you in rainbow colours: solid, neon outline, glow, or real frozen copies of you. The engine gets your outline
// from a segmentation model; here it comes from the picture's transparency, a learned empty room, or movement.
// Every frame a quarter-size snapshot (picture + silhouette) goes into a ring; the ghosts are painted oldest first.

#include <obs-module.h>
#include <graphics/vec2.h>
#include <graphics/vec4.h>
#include <math.h>
#include <string.h>

#include "trail-effect.h" // TRAIL_EFFECT: src/trail.effect as a string, made by build.sh

#define MAX_GHOSTS 12
#define MAX_GAP 8
#define RING (MAX_GHOSTS * MAX_GAP + 1)

enum { FROM_MOVE, FROM_ALPHA, FROM_ROOM };

struct trail {
	obs_source_t *ctx;
	gs_effect_t *effect;
	gs_texrender_t *cur, *bg, *acc;
	gs_texrender_t *ring[RING]; // snapshots, made on first use
	int filled;                 // how many slots hold a real snapshot
	uint32_t w, h, sw, sh;
	int slot;
	bool rendered, has_bg, learn, forget;
	float t, dt;

	int from, ghosts, gap, style;
	float fade, soft, hue, spread, spin, tol;
};

static const char *trail_name(void *unused)
{
	UNUSED_PARAMETER(unused);
	return "Rainbow Trail";
}

static void trail_update(void *data, obs_data_t *s)
{
	struct trail *f = data;
	f->from = (int)obs_data_get_int(s, "from");
	f->ghosts = (int)obs_data_get_int(s, "ghosts");
	f->gap = (int)obs_data_get_int(s, "gap");
	f->style = (int)obs_data_get_int(s, "style");
	f->fade = (float)obs_data_get_double(s, "fade");
	f->soft = (float)obs_data_get_double(s, "soft");
	f->hue = (float)obs_data_get_double(s, "hue");
	f->spread = (float)obs_data_get_double(s, "spread");
	f->spin = (float)obs_data_get_double(s, "spin");
	f->tol = (float)obs_data_get_double(s, "tol");
}

// the defaults are time.py's RAINBOW TRAIL ones
static void trail_defaults(obs_data_t *s)
{
	obs_data_set_default_int(s, "from", FROM_MOVE);
	obs_data_set_default_int(s, "ghosts", 6);
	obs_data_set_default_int(s, "gap", 3);
	obs_data_set_default_int(s, "style", 0);
	obs_data_set_default_double(s, "fade", 0.85);
	obs_data_set_default_double(s, "soft", 0.35);
	obs_data_set_default_double(s, "hue", 0.0);
	obs_data_set_default_double(s, "spread", 1.0);
	obs_data_set_default_double(s, "spin", 0.25);
	obs_data_set_default_double(s, "tol", 0.08);
}

static bool learn_clicked(obs_properties_t *props, obs_property_t *p, void *data)
{
	UNUSED_PARAMETER(props);
	UNUSED_PARAMETER(p);
	((struct trail *)data)->learn = true;
	return false;
}

static bool forget_clicked(obs_properties_t *props, obs_property_t *p, void *data)
{
	UNUSED_PARAMETER(props);
	UNUSED_PARAMETER(p);
	((struct trail *)data)->forget = true;
	return false;
}

static bool from_changed(obs_properties_t *p, obs_property_t *unused, obs_data_t *s)
{
	UNUSED_PARAMETER(unused);
	const int from = (int)obs_data_get_int(s, "from");
	obs_property_set_visible(obs_properties_get(p, "learn"), from == FROM_ROOM);
	obs_property_set_visible(obs_properties_get(p, "forget"), from == FROM_ROOM);
	obs_property_set_visible(obs_properties_get(p, "tol"), from != FROM_ALPHA);
	return true;
}

static obs_properties_t *trail_properties(void *data)
{
	UNUSED_PARAMETER(data);
	obs_properties_t *p = obs_properties_create();
	obs_property_t *fr = obs_properties_add_list(p, "from", "Your outline from", OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_INT);
	obs_property_list_add_int(fr, "Movement (no setup)", FROM_MOVE);
	obs_property_list_add_int(fr, "Transparency (put Chroma Key / background removal above this filter)", FROM_ALPHA);
	obs_property_list_add_int(fr, "Learned empty room", FROM_ROOM);
	obs_property_set_modified_callback(fr, from_changed);
	obs_properties_add_button(p, "learn", "Learn background (step out of the picture first)", learn_clicked);
	obs_properties_add_button(p, "forget", "Forget background", forget_clicked);
	obs_properties_add_float_slider(p, "tol", "Sensitivity (lower = picks up smaller changes)", 0.02, 0.5, 0.01);
	obs_property_t *st = obs_properties_add_list(p, "style", "Style", OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_INT);
	obs_property_list_add_int(st, "Solid", 0);
	obs_property_list_add_int(st, "Outline", 1);
	obs_property_list_add_int(st, "Glow", 2);
	obs_property_list_add_int(st, "Real (copies of you)", 3);
	obs_properties_add_int_slider(p, "ghosts", "Ghosts", 2, MAX_GHOSTS, 1);
	obs_properties_add_int_slider(p, "gap", "Frames apart", 1, MAX_GAP, 1);
	obs_properties_add_float_slider(p, "fade", "Fade", 0.2, 1.0, 0.01);
	obs_properties_add_float_slider(p, "soft", "Edge softness", 0.0, 1.0, 0.01);
	obs_properties_add_float_slider(p, "hue", "Colour", 0.0, 1.0, 0.01);
	obs_properties_add_float_slider(p, "spread", "Rainbow spread", 0.0, 1.0, 0.01);
	obs_properties_add_float_slider(p, "spin", "Colour spin", 0.0, 1.0, 0.01);
	return p;
}

static void *trail_create(obs_data_t *settings, obs_source_t *source)
{
	struct trail *f = bzalloc(sizeof(*f));
	f->ctx = source;
	f->dt = 1.0f / 30.0f;
	obs_enter_graphics();
	char *err = NULL;
	f->effect = gs_effect_create(TRAIL_EFFECT, "trail.effect", &err);
	if (!f->effect)
		blog(LOG_ERROR, "[rainbow trail] effect failed to compile: %s", err ? err : "?");
	bfree(err);
	f->cur = gs_texrender_create(GS_RGBA, GS_ZS_NONE);
	f->bg = gs_texrender_create(GS_RGBA, GS_ZS_NONE);
	f->acc = gs_texrender_create(GS_RGBA16F, GS_ZS_NONE);
	obs_leave_graphics();
	trail_update(f, settings);
	return f;
}

static void trail_destroy(void *data)
{
	struct trail *f = data;
	obs_enter_graphics();
	gs_effect_destroy(f->effect);
	gs_texrender_destroy(f->cur);
	gs_texrender_destroy(f->bg);
	gs_texrender_destroy(f->acc);
	for (int i = 0; i < RING; i++)
		gs_texrender_destroy(f->ring[i]);
	obs_leave_graphics();
	bfree(f);
}

static void trail_tick(void *data, float seconds)
{
	struct trail *f = data;
	f->rendered = false;
	f->dt = seconds > 0.0f && seconds < 0.25f ? seconds : 1.0f / 30.0f;
	f->t += f->dt;
}

static void set_tex(struct trail *f, const char *name, gs_texrender_t *tr)
{
	gs_effect_set_texture(gs_effect_get_param_by_name(f->effect, name), gs_texrender_get_texture(tr));
}

static void set_float(struct trail *f, const char *name, float v)
{
	gs_effect_set_float(gs_effect_get_param_by_name(f->effect, name), v);
}

static void set_vec2(struct trail *f, const char *name, float x, float y)
{
	struct vec2 v;
	vec2_set(&v, x, y);
	gs_effect_set_vec2(gs_effect_get_param_by_name(f->effect, name), &v);
}

static void pass(struct trail *f, gs_texrender_t *dst, uint32_t w, uint32_t h, const char *tech)
{
	gs_texrender_reset(dst);
	if (!gs_texrender_begin(dst, w, h))
		return;
	gs_ortho(0.0f, (float)w, 0.0f, (float)h, -100.0f, 100.0f);
	while (gs_effect_loop(f->effect, tech))
		gs_draw_sprite(NULL, 0, w, h);
	gs_texrender_end(dst);
}

static bool capture_source(struct trail *f, uint32_t w, uint32_t h)
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

// time.py _rainbow: ghost i at hue + SPREAD * i / n around the colour wheel
static void rainbow(float hue6, struct vec4 *c)
{
	const float k[3] = {3.0f, 2.0f, 4.0f}, b[3] = {-1.0f, 2.0f, 2.0f}, s[3] = {1.0f, -1.0f, -1.0f};
	float h = fmodf(hue6, 6.0f);
	if (h < 0.0f)
		h += 6.0f;
	float v[3];
	for (int j = 0; j < 3; j++)
		v[j] = fminf(1.0f, fmaxf(0.0f, b[j] + fabsf(h - k[j]) * s[j]));
	c->x = v[0];
	c->y = v[1];
	c->z = v[2];
}

static void trail_step(struct trail *f, uint32_t w, uint32_t h)
{
	if (w != f->w || h != f->h) {
		f->w = w;
		f->h = h;
		f->sw = w / 4 > 16 ? w / 4 : 16;
		f->sh = h / 4 > 16 ? h / 4 : 16;
		f->filled = 0;
		f->has_bg = false;
	}
	if (!capture_source(f, w, h))
		return;
	const int n = f->ghosts < 2 ? 2 : f->ghosts > MAX_GHOSTS ? MAX_GHOSTS : f->ghosts;
	const int gap = f->gap < 1 ? 1 : f->gap > MAX_GAP ? MAX_GAP : f->gap;
	const int need = n * gap + 1;

	// the newest snapshot goes in at a falling slot index, so ghost i (i frames back) is slot + i
	const int prev = f->slot;
	f->slot = (f->slot + RING - 1) % RING;
	if (!f->ring[f->slot])
		f->ring[f->slot] = gs_texrender_create(GS_RGBA, GS_ZS_NONE);

	if (f->forget)
		f->has_bg = f->forget = false;
	if (f->learn) { // the empty room: a snapshot with nothing compared
		f->learn = false;
		set_tex(f, "image", f->cur);
		set_vec2(f, "cell", 1.0f / f->sw, 1.0f / f->sh);
		set_float(f, "from", (float)FROM_MOVE);
		set_float(f, "has_prev", 0.0f);
		pass(f, f->bg, f->sw, f->sh, "Small");
		f->has_bg = true;
	}
	set_tex(f, "image", f->cur);
	gs_texrender_t *cmp = f->from == FROM_ROOM ? (f->has_bg ? f->bg : NULL) : (f->filled > 0 ? f->ring[prev] : NULL);
	set_tex(f, "prev_tex", cmp ? cmp : f->cur);
	set_vec2(f, "cell", 1.0f / f->sw, 1.0f / f->sh);
	set_float(f, "from", (float)f->from);
	set_float(f, "has_prev", cmp ? 1.0f : 0.0f);
	set_float(f, "tol", f->tol);
	pass(f, f->ring[f->slot], f->sw, f->sh, "Small");
	if (f->filled < RING)
		f->filled++;

	// paint the ghosts that exist, oldest first, onto the cleared trail
	gs_texrender_reset(f->acc);
	if (!gs_texrender_begin(f->acc, f->sw, f->sh))
		return;
	struct vec4 clear;
	vec4_zero(&clear);
	gs_clear(GS_CLEAR_COLOR, &clear, 0.0f, 0);
	gs_ortho(0.0f, (float)f->sw, 0.0f, (float)f->sh, -100.0f, 100.0f);
	gs_enable_blending(true);
	if (f->style == 2)
		gs_blend_function(GS_BLEND_ONE, GS_BLEND_ONE);
	else
		gs_blend_function_separate(GS_BLEND_SRCALPHA, GS_BLEND_INVSRCALPHA, GS_BLEND_ONE, GS_BLEND_INVSRCALPHA);
	const float hue6 = fmodf(f->hue + f->t * f->spin, 1.0f) * 6.0f;
	for (int i = n; i >= 1; i--) {
		const int back = i * gap;
		if (back >= f->filled || back >= need)
			continue;
		gs_texrender_t *g = f->ring[(f->slot + back) % RING];
		if (!g)
			continue;
		struct vec4 c;
		rainbow(hue6 + (float)i * 6.0f * f->spread / (float)n, &c);
		c.w = powf(f->fade, (float)i);
		set_tex(f, "image", g);
		gs_effect_set_vec4(gs_effect_get_param_by_name(f->effect, "color"), &c);
		set_vec2(f, "texel", 1.0f / f->sw, 1.0f / f->sh);
		set_float(f, "style", (float)f->style);
		set_float(f, "soft", f->soft);
		while (gs_effect_loop(f->effect, "Ghost"))
			gs_draw_sprite(NULL, 0, f->sw, f->sh);
	}
	gs_texrender_end(f->acc);
}

static void trail_render(void *data, gs_effect_t *unused)
{
	UNUSED_PARAMETER(unused);
	struct trail *f = data;
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
		gs_blend_state_push();
		gs_enable_blending(false);
		trail_step(f, w, h);
		gs_blend_state_pop();
	}
	if (!f->filled || !f->ring[f->slot]) {
		gs_enable_framebuffer_srgb(srgb);
		return;
	}
	set_tex(f, "image", f->cur);
	set_tex(f, "prev_tex", f->acc);
	set_tex(f, "now_tex", f->ring[f->slot]);
	set_float(f, "from", (float)f->from);
	set_float(f, "style", (float)f->style);
	while (gs_effect_loop(f->effect, "Out"))
		gs_draw_sprite(NULL, 0, w, h);
	gs_enable_framebuffer_srgb(srgb);
}

struct obs_source_info trail_filter = {
	.id = "rainbow_trail_filter",
	.type = OBS_SOURCE_TYPE_FILTER,
	.output_flags = OBS_SOURCE_VIDEO,
	.get_name = trail_name,
	.create = trail_create,
	.destroy = trail_destroy,
	.update = trail_update,
	.get_defaults = trail_defaults,
	.get_properties = trail_properties,
	.video_tick = trail_tick,
	.video_render = trail_render,
};
