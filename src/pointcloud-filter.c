// Point Cloud filter for OBS. A port of moshcap's engine/fx/pointcloud.py for a plain webcam: no depth camera,
// no neural net. The GPU guesses a depth per dot from the picture (pointcloud.effect), the CPU reads that small
// grid back, turns every cell into a 3D point seen from a slowly orbiting camera, and the GPU draws them.

#include <obs-module.h>
#include <graphics/vec2.h>
#include <graphics/vec3.h>
#include <graphics/vec4.h>
#include <math.h>
#include <stdlib.h>

#include "pointcloud-effect.h" // POINTCLOUD_EFFECT: src/pointcloud.effect as a string, made by build.sh

#define MAX_PTS 130000 // dots from the picture (1080p at spacing 4)
#define MAX_DUST 30000
#define Z_NEAR 1.5f
#define PI 3.14159265f

struct dust {
	float p[3], v[3], c[3], life;
};

struct pc {
	obs_source_t *ctx;
	gs_effect_t *effect;
	gs_texrender_t *cur;       // this frame of the source
	gs_texrender_t *raw;       // the source at one texel per dot
	gs_texrender_t *bg;        // the empty room, learned on request
	gs_texrender_t *dep[2];    // depth guess + "not the room": [0] latest
	gs_texrender_t *pack;      // what the CPU reads
	gs_texrender_t *pts;       // one-pixel points
	gs_texrender_t *acc[2];    // grown dots with trails: [0] latest
	gs_texrender_t *glw[2];    // quarter-size glow
	gs_stagesurf_t *stage[2];  // readback, a frame apart so the GPU never waits
	gs_vertbuffer_t *vb;
	uint8_t *prevd;            // last frame's depth byte per cell (for dust)
	struct dust *dust;
	int ndust, cand_last, npts;
	uint32_t w, h, cw, ch, gw, gh;
	bool rendered, have_dep, have_trail, stage_ok, prev_ok, has_bg, learn, forget;
	uint32_t rng;
	float t, dt, pivz;

	int density, color;
	float size, glow, light, depth, bright, bulge, steady, cut, orbit, orbit_s, dustamt, wind, fade, bgmix, gain, bg_tol, sparkle;
	bool transparent, hide;
};

static const char *pc_name(void *unused)
{
	UNUSED_PARAMETER(unused);
	return "Point Cloud";
}

static void pc_update(void *data, obs_data_t *s)
{
	struct pc *f = data;
	f->density = (int)obs_data_get_int(s, "density");
	f->color = (int)obs_data_get_int(s, "color");
	f->size = (float)obs_data_get_double(s, "size");
	f->gain = (float)obs_data_get_double(s, "gain");
	f->glow = (float)obs_data_get_double(s, "glow");
	f->light = (float)obs_data_get_double(s, "light");
	f->depth = (float)obs_data_get_double(s, "depth");
	f->bright = (float)obs_data_get_double(s, "bright");
	f->bulge = (float)obs_data_get_double(s, "bulge");
	f->steady = (float)obs_data_get_double(s, "steady");
	f->cut = (float)obs_data_get_double(s, "cut");
	f->orbit = (float)obs_data_get_double(s, "orbit");
	f->orbit_s = (float)obs_data_get_double(s, "orbit_s");
	f->dustamt = (float)obs_data_get_double(s, "dust");
	f->wind = (float)obs_data_get_double(s, "wind");
	f->fade = (float)obs_data_get_double(s, "fade");
	f->bgmix = (float)obs_data_get_double(s, "bg");
	f->bg_tol = (float)obs_data_get_double(s, "bg_tol");
	f->sparkle = (float)obs_data_get_double(s, "sparkle");
	f->transparent = obs_data_get_bool(s, "transparent");
	f->hide = obs_data_get_bool(s, "hide");
}

static void pc_defaults(obs_data_t *s)
{
	obs_data_set_default_int(s, "density", 6);
	obs_data_set_default_int(s, "color", 0);
	obs_data_set_default_double(s, "size", 1.5);
	obs_data_set_default_double(s, "gain", 1.0);
	obs_data_set_default_double(s, "glow", 0.7);
	obs_data_set_default_double(s, "light", 0.6);
	obs_data_set_default_double(s, "depth", 0.8);
	obs_data_set_default_double(s, "bright", 0.7);
	obs_data_set_default_double(s, "bulge", 0.5);
	obs_data_set_default_double(s, "steady", 0.6);
	obs_data_set_default_double(s, "cut", 0.0);
	obs_data_set_default_double(s, "orbit", 12.0);
	obs_data_set_default_double(s, "orbit_s", 0.12);
	obs_data_set_default_double(s, "dust", 0.4);
	obs_data_set_default_double(s, "wind", 0.5);
	obs_data_set_default_double(s, "fade", 0.55);
	obs_data_set_default_double(s, "bg", 0.0);
	obs_data_set_default_double(s, "bg_tol", 0.07);
	obs_data_set_default_double(s, "sparkle", 0.25);
	obs_data_set_default_bool(s, "transparent", false);
	obs_data_set_default_bool(s, "hide", true);
}

static bool pc_learn_clicked(obs_properties_t *props, obs_property_t *p, void *data)
{
	UNUSED_PARAMETER(props);
	UNUSED_PARAMETER(p);
	((struct pc *)data)->learn = true;
	return false;
}

static bool pc_forget_clicked(obs_properties_t *props, obs_property_t *p, void *data)
{
	UNUSED_PARAMETER(props);
	UNUSED_PARAMETER(p);
	((struct pc *)data)->forget = true;
	return false;
}

static obs_properties_t *pc_properties(void *data)
{
	UNUSED_PARAMETER(data);
	obs_properties_t *p = obs_properties_create();
	obs_properties_add_int_slider(p, "density", "Dot spacing (lower = more dots)", 3, 16, 1);
	obs_properties_add_float_slider(p, "size", "Dot size", 0.5, 4.0, 0.05);
	obs_properties_add_float_slider(p, "gain", "Brightness", 0.2, 3.0, 0.05);
	obs_properties_add_float_slider(p, "glow", "Glow", 0.0, 2.0, 0.05);
	obs_properties_add_float_slider(p, "sparkle", "Sparkle", 0.0, 1.0, 0.01);
	obs_property_t *c = obs_properties_add_list(p, "color", "Colour", OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_INT);
	obs_property_list_add_int(c, "White", 0);
	obs_property_list_add_int(c, "Cyan", 1);
	obs_property_list_add_int(c, "Picture", 2);
	obs_properties_add_float_slider(p, "light", "Side light", 0.0, 1.0, 0.01);
	obs_properties_add_float_slider(p, "depth", "Depth (0 = flat)", 0.0, 2.0, 0.05);
	obs_properties_add_float_slider(p, "bright", "Bright is near (negative: dark is near)", -1.0, 1.0, 0.05);
	obs_properties_add_float_slider(p, "bulge", "Middle comes forward", 0.0, 1.0, 0.05);
	obs_properties_add_float_slider(p, "steady", "Steady depth", 0.0, 0.95, 0.01);
	obs_properties_add_float_slider(p, "cut", "Hide the farthest", 0.0, 1.0, 0.01);
	obs_properties_add_float_slider(p, "orbit", "Orbit degrees", 0.0, 45.0, 0.5);
	obs_properties_add_float_slider(p, "orbit_s", "Orbit speed", 0.0, 1.0, 0.01);
	obs_properties_add_float_slider(p, "dust", "Dust off movement", 0.0, 2.0, 0.05);
	obs_properties_add_float_slider(p, "wind", "Wind", -2.0, 2.0, 0.05);
	obs_properties_add_float_slider(p, "fade", "Trails", 0.0, 0.98, 0.01);
	obs_properties_add_float_slider(p, "bg", "Picture behind", 0.0, 1.0, 0.01);
	obs_properties_add_bool(p, "transparent", "Transparent where there are no dots");
	obs_properties_add_button(p, "learn", "Learn background (step out of the picture first)", pc_learn_clicked);
	obs_properties_add_button(p, "forget", "Forget background", pc_forget_clicked);
	obs_properties_add_bool(p, "hide", "Dots on you only (after Learn background)");
	obs_properties_add_float_slider(p, "bg_tol", "Background tolerance", 0.02, 0.5, 0.01);
	return p;
}

static void *pc_create(obs_data_t *settings, obs_source_t *source)
{
	struct pc *f = bzalloc(sizeof(*f));
	f->ctx = source;
	f->rng = 0x9E3779B9u;
	f->pivz = Z_NEAR + 0.4f;
	f->dust = bmalloc(sizeof(struct dust) * MAX_DUST);

	obs_enter_graphics();
	char *err = NULL;
	f->effect = gs_effect_create(POINTCLOUD_EFFECT, "pointcloud.effect", &err);
	if (!f->effect)
		blog(LOG_ERROR, "[pointcloud] effect failed to compile: %s", err ? err : "?");
	bfree(err);
	f->cur = gs_texrender_create(GS_RGBA, GS_ZS_NONE);
	f->raw = gs_texrender_create(GS_RGBA, GS_ZS_NONE);
	f->bg = gs_texrender_create(GS_RGBA, GS_ZS_NONE);
	f->dep[0] = gs_texrender_create(GS_RGBA16F, GS_ZS_NONE);
	f->dep[1] = gs_texrender_create(GS_RGBA16F, GS_ZS_NONE);
	f->pack = gs_texrender_create(GS_RGBA, GS_ZS_NONE);
	f->pts = gs_texrender_create(GS_RGBA16F, GS_ZS_NONE);
	f->acc[0] = gs_texrender_create(GS_RGBA16F, GS_ZS_NONE);
	f->acc[1] = gs_texrender_create(GS_RGBA16F, GS_ZS_NONE);
	f->glw[0] = gs_texrender_create(GS_RGBA16F, GS_ZS_NONE);
	f->glw[1] = gs_texrender_create(GS_RGBA16F, GS_ZS_NONE);

	struct gs_vb_data *vd = gs_vbdata_create();
	vd->num = MAX_PTS + MAX_DUST;
	vd->points = bzalloc(sizeof(struct vec3) * vd->num);
	vd->colors = bzalloc(sizeof(uint32_t) * vd->num);
	f->vb = gs_vertexbuffer_create(vd, GS_DYNAMIC);
	obs_leave_graphics();

	pc_update(f, settings);
	return f;
}

static void pc_destroy(void *data)
{
	struct pc *f = data;
	obs_enter_graphics();
	gs_effect_destroy(f->effect);
	gs_texrender_destroy(f->cur);
	gs_texrender_destroy(f->raw);
	gs_texrender_destroy(f->bg);
	gs_texrender_destroy(f->pack);
	gs_texrender_destroy(f->pts);
	for (int i = 0; i < 2; i++) {
		gs_texrender_destroy(f->dep[i]);
		gs_texrender_destroy(f->acc[i]);
		gs_texrender_destroy(f->glw[i]);
		gs_stagesurface_destroy(f->stage[i]);
	}
	gs_vertexbuffer_destroy(f->vb);
	obs_leave_graphics();
	bfree(f->prevd);
	bfree(f->dust);
	bfree(f);
}

static void pc_tick(void *data, float seconds)
{
	struct pc *f = data;
	f->rendered = false;
	f->t += seconds;
	f->dt = seconds > 0.0f && seconds < 0.25f ? seconds : 1.0f / 30.0f;
}

static void set_tex(struct pc *f, const char *name, gs_texrender_t *tr)
{
	gs_effect_set_texture(gs_effect_get_param_by_name(f->effect, name), gs_texrender_get_texture(tr));
}

static void set_vec2(struct pc *f, const char *name, float x, float y)
{
	struct vec2 v;
	vec2_set(&v, x, y);
	gs_effect_set_vec2(gs_effect_get_param_by_name(f->effect, name), &v);
}

static void set_float(struct pc *f, const char *name, float v)
{
	gs_effect_set_float(gs_effect_get_param_by_name(f->effect, name), v);
}

// one full-screen pass of technique tech into dst at w x h (params already set)
static void pass(struct pc *f, gs_texrender_t *dst, uint32_t w, uint32_t h, const char *tech)
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

static bool capture_source(struct pc *f, uint32_t w, uint32_t h)
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

static inline float frand(struct pc *f) // 0..1
{
	uint32_t x = f->rng;
	x ^= x << 13;
	x ^= x >> 17;
	x ^= x << 5;
	f->rng = x;
	return (float)(x >> 8) * (1.0f / 16777216.0f);
}

static inline float frandn(struct pc *f) // roughly normal, sd 1
{
	return (frand(f) + frand(f) + frand(f) + frand(f) - 2.0f) * 1.7320508f;
}

static inline uint32_t rgb_bytes(float r, float g, float b)
{
	uint32_t R = r >= 255.0f ? 255u : (uint32_t)r, G = g >= 255.0f ? 255u : (uint32_t)g, B = b >= 255.0f ? 255u : (uint32_t)b;
	return 0xFF000000u | (B << 16) | (G << 8) | R;
}

// the cells the GPU packed (rgba8, a = depth, 0 = no dot) become points seen from the orbiting camera
static void build_points(struct pc *f, const uint8_t *px, uint32_t linesize)
{
	struct gs_vb_data *vd = gs_vertexbuffer_get_data(f->vb);
	struct vec3 *pos = vd->points;
	uint32_t *col = vd->colors;
	const float cw = (float)f->cw, ch = (float)f->ch;
	const float fl = cw * 0.9f, cx = cw * 0.5f, cy = ch * 0.5f; // ~58 deg across: close enough for parallax
	const float sx = cw / f->gw, sy = ch / f->gh;
	const float dt = f->dt;

	const float a = f->orbit * PI / 180.0f, ph = 2.0f * PI * f->orbit_s * f->t;
	const float yaw = a * sinf(ph), pitch = a * 0.25f * sinf(ph * 0.61f + 1.0f);
	const float cyw = cosf(yaw), syw = sinf(yaw), cp = cosf(pitch), sp = sinf(pitch);
	const float pz = f->pivz;

#define PROJECT(X, Y, Z, VALUE)                                                    \
	do {                                                                       \
		float x_ = (X), y_ = (Y), z_ = (Z)-pz;                             \
		float qx = cyw * x_ + syw * z_;                                    \
		float qy = sp * syw * x_ + cp * y_ - sp * cyw * z_;                \
		float qz = -cp * syw * x_ + sp * y_ + cp * cyw * z_ + pz;          \
		if (qz > 0.05f) {                                                  \
			float u_ = qx * fl / qz + cx, v_ = qy * fl / qz + cy;      \
			if (u_ >= 0.0f && u_ < cw && v_ >= 0.0f && v_ < ch) {      \
				vec3_set(&pos[n], floorf(u_) + 0.5f, floorf(v_) + 0.5f, 0.0f); \
				col[n++] = (VALUE);                                \
			}                                                          \
		}                                                                  \
	} while (0)

	int n = 0, cand = 0, spawned = 0;
	const float budget = 1500.0f * f->dustamt * dt * 30.0f;
	const float accept = f->cand_last > 0 ? budget / (float)f->cand_last : 1.0f;
	const bool dusty = f->dustamt > 0.0f && f->prev_ok;
	double zsum = 0.0;
	int zn = 0;

	for (uint32_t j = 0; j < f->gh; j++) {
		const uint8_t *row = px + (size_t)j * linesize;
		uint8_t *prow = f->prevd + (size_t)j * f->gw;
		for (uint32_t i = 0; i < f->gw; i++) {
			const uint8_t *c = row + i * 4;
			const uint8_t da = c[3], was = prow[i];
			prow[i] = da;
			if (!da)
				continue;
			const float z = Z_NEAR + (1.0f - (float)(da - 1) / 254.0f) * f->depth;
			// scattered inside its cell (its own fixed spot, so a dot is a thing you can follow), plus a fresh
			// shake every frame: the sparkle
			uint32_t hsh = (i * 0x9E3779B1u) ^ (j * 0x85EBCA77u);
			hsh ^= hsh >> 15;
			hsh *= 0x2C1B3C6Du;
			hsh ^= hsh >> 12;
			const float jx = (float)(hsh & 0xFFFF) / 65536.0f + (frand(f) - 0.5f) * f->sparkle;
			const float jy = (float)(hsh >> 16) / 65536.0f + (frand(f) - 0.5f) * f->sparkle;
			const float X = (((float)i + jx) * sx - cx) * z / fl;
			const float Y = (((float)j + jy) * sy - cy) * z / fl;
			zsum += z;
			zn++;
			if (n < MAX_PTS)
				PROJECT(X, Y, z, 0xFF000000u | ((uint32_t)c[2] << 16) | ((uint32_t)c[1] << 8) | c[0]);
			// dust: where the depth jumped since the last frame, points break off
			if (dusty && was && abs((int)da - (int)was) > 14) {
				cand++;
				if ((float)spawned < budget && f->ndust < MAX_DUST && frand(f) < accept) {
					struct dust *d = &f->dust[f->ndust++];
					d->p[0] = X;
					d->p[1] = Y;
					d->p[2] = z;
					d->v[0] = frandn(f) * 0.25f;
					d->v[1] = frandn(f) * 0.25f - 0.15f; // a little lift
					d->v[2] = frandn(f) * 0.25f;
					d->c[0] = c[0] * 0.9f;
					d->c[1] = c[1] * 0.9f;
					d->c[2] = c[2] * 0.9f;
					d->life = 1.0f;
					spawned++;
				}
			}
		}
	}
	f->cand_last = cand;
	f->prev_ok = true;
	if (zn)
		f->pivz += ((float)(zsum / zn) - f->pivz) * 0.08f; // orbit around the middle of what is there

	const float decay = dt / (0.6f + 2.4f * f->fade), drag = 1.0f - 0.8f * dt;
	for (int k = 0; k < f->ndust;) {
		struct dust *d = &f->dust[k];
		d->life -= decay;
		if (d->life <= 0.0f) {
			*d = f->dust[--f->ndust];
			continue;
		}
		d->v[0] += f->wind * dt * 1.5f;
		for (int q = 0; q < 3; q++) {
			d->v[q] = (d->v[q] + frandn(f) * 0.6f * dt) * drag; // swirl
			d->p[q] += d->v[q] * dt;
		}
		const float l = d->life * sqrtf(d->life) * 1.4f;
		PROJECT(d->p[0], d->p[1], d->p[2], rgb_bytes(d->c[0] * l, d->c[1] * l, d->c[2] * l));
		k++;
	}
#undef PROJECT
	f->npts = n;
}

static void resize(struct pc *f, uint32_t w, uint32_t h)
{
	f->w = w;
	f->h = h;
	const uint32_t k = h > 1200 ? 2 : 1; // above 1080p the cloud is drawn at half size
	f->cw = w / k > 0 ? w / k : 1;
	f->ch = h / k > 0 ? h / k : 1;
}

// the dot grid follows the spacing slider (given for a 1080-line picture), capped at MAX_PTS
static void regrid(struct pc *f)
{
	float s = (float)f->density * (float)f->ch / 1080.0f;
	if (s < 2.0f)
		s = 2.0f;
	uint32_t gw, gh;
	for (;; s *= 1.1f) {
		gw = (uint32_t)((float)f->cw / s);
		gh = (uint32_t)((float)f->ch / s);
		if (gw < 4)
			gw = 4;
		if (gh < 4)
			gh = 4;
		if (gw * gh <= MAX_PTS)
			break;
	}
	if (gw == f->gw && gh == f->gh && f->prevd)
		return;
	f->gw = gw;
	f->gh = gh;
	bfree(f->prevd);
	f->prevd = bzalloc((size_t)gw * gh);
	for (int i = 0; i < 2; i++) {
		gs_stagesurface_destroy(f->stage[i]);
		f->stage[i] = gs_stagesurface_create(gw, gh, GS_RGBA);
	}
	f->stage_ok = f->prev_ok = f->have_dep = false;
	f->npts = 0;
}

// advance one frame: capture, guess depth, read last frame's grid back, draw the points, grow + trail + glow
static void pc_step(struct pc *f, uint32_t w, uint32_t h)
{
	if (w != f->w || h != f->h) {
		resize(f, w, h);
		f->have_trail = f->has_bg = false;
	}
	regrid(f);
	if (!capture_source(f, w, h))
		return;
	const uint32_t cw = f->cw, ch = f->ch, gw = f->gw, gh = f->gh;

	set_tex(f, "image", f->cur);
	set_vec2(f, "cell", 1.0f / gw, 1.0f / gh);
	pass(f, f->raw, gw, gh, "Down");

	// OBS forgets every effect parameter when a technique ends: each pass sets all it uses
	if (f->forget)
		f->has_bg = f->forget = false;
	if (f->learn) {
		f->learn = false;
		f->has_bg = true;
		set_tex(f, "image", f->raw);
		set_vec2(f, "texel", 0.0f, 0.0f); // a plain copy: blurring it would make every hard edge of the room differ
		pass(f, f->bg, gw, gh, "GlowDown");
	}

	swap(&f->dep[0], &f->dep[1]);
	set_tex(f, "image", f->raw);
	set_vec2(f, "texel", 1.0f / gw, 1.0f / gh);
	set_tex(f, "prev_tex", f->have_dep ? f->dep[1] : f->raw);
	set_tex(f, "aux_tex", f->has_bg ? f->bg : f->raw);
	set_float(f, "bright", f->bright);
	set_float(f, "bulge", f->bulge);
	set_float(f, "has_bg", f->has_bg ? 1.0f : 0.0f);
	set_float(f, "bg_tol", f->bg_tol);
	set_float(f, "steady", f->have_dep ? powf(f->steady, f->dt * 30.0f) : 0.0f);
	pass(f, f->dep[0], gw, gh, "Depth");
	f->have_dep = true;

	set_tex(f, "image", f->raw);
	set_tex(f, "prev_tex", f->dep[0]);
	set_vec2(f, "texel", 1.0f / gw, 1.0f / gh);
	set_float(f, "color_mode", (float)f->color);
	set_float(f, "light", f->light);
	// the depth slope across one cell, in the engine's units, toned down: a guessed depth is rougher than a measured one
	set_float(f, "light_k", 0.15f * f->depth * 0.9f * (float)gw / (2.0f * (Z_NEAR + f->depth * 0.5f)));
	set_float(f, "cut", f->cut);
	set_float(f, "hide", f->hide && f->has_bg ? 1.0f : 0.0f);
	pass(f, f->pack, gw, gh, "Pack");

	// read the grid packed a frame ago (no waiting on the GPU), queue this one
	if (f->stage[0] && f->stage[1]) {
		uint8_t *px;
		uint32_t linesize;
		if (f->stage_ok && gs_stagesurface_map(f->stage[0], &px, &linesize)) {
			build_points(f, px, linesize);
			gs_stagesurface_unmap(f->stage[0]);
		}
		gs_stage_texture(f->stage[1], gs_texrender_get_texture(f->pack));
		gs_stagesurf_t *t = f->stage[0];
		f->stage[0] = f->stage[1]; // [0]: the one just queued, read next frame
		f->stage[1] = t;
		f->stage_ok = true;
	}

	gs_texrender_reset(f->pts);
	if (gs_texrender_begin(f->pts, cw, ch)) {
		struct vec4 clear;
		vec4_zero(&clear);
		gs_clear(GS_CLEAR_COLOR, &clear, 0.0f, 0);
		gs_ortho(0.0f, (float)cw, 0.0f, (float)ch, -100.0f, 100.0f);
		if (f->npts > 0) {
			gs_blend_function(GS_BLEND_ONE, GS_BLEND_ONE); // dots add up
			// a sparser grid means each dot carries more light (2x undoes the halving in Pack)
			set_float(f, "gain", 2.0f * f->gain * ((float)cw / (float)gw) / 4.0f);
			gs_vertexbuffer_flush(f->vb);
			gs_load_vertexbuffer(f->vb);
			gs_load_indexbuffer(NULL);
			while (gs_effect_loop(f->effect, "Points"))
				gs_draw(GS_POINTS, 0, (uint32_t)f->npts);
			gs_load_vertexbuffer(NULL);
			gs_blend_function(GS_BLEND_ONE, GS_BLEND_ZERO);
		}
		gs_texrender_end(f->pts);
	}

	swap(&f->acc[0], &f->acc[1]);
	float r = f->size * 0.9f * (float)ch / 1080.0f;
	if (r < 0.0f)
		r = 0.0f;
	if (r > 4.0f)
		r = 4.0f;
	set_tex(f, "image", f->pts);
	set_tex(f, "prev_tex", f->have_trail ? f->acc[1] : f->pts);
	set_vec2(f, "texel", 1.0f / cw, 1.0f / ch);
	gs_effect_set_int(gs_effect_get_param_by_name(f->effect, "rad"), (int)ceilf(r));
	set_float(f, "rad2", r * r + 0.5f);
	set_float(f, "fade", f->have_trail ? powf(f->fade, f->dt * 30.0f) : 0.0f);
	pass(f, f->acc[0], cw, ch, "Dots");
	f->have_trail = true;

	const uint32_t qw = (cw + 3) / 4, qh = (ch + 3) / 4;
	set_tex(f, "image", f->acc[0]);
	set_vec2(f, "texel", 1.0f / cw, 1.0f / ch);
	pass(f, f->glw[0], qw, qh, "GlowDown");
	for (int i = 0; i < 2; i++) {
		set_tex(f, "image", f->glw[i]);
		set_vec2(f, "texel", 1.0f / qw, 1.0f / qh);
		pass(f, f->glw[1 - i], qw, qh, "Blur");
	}
}

static void pc_render(void *data, gs_effect_t *unused)
{
	UNUSED_PARAMETER(unused);
	struct pc *f = data;
	obs_source_t *target = obs_filter_get_target(f->ctx);
	uint32_t w = target ? obs_source_get_base_width(target) : 0;
	uint32_t h = target ? obs_source_get_base_height(target) : 0;
	if (!f->effect || !w || !h) {
		obs_source_skip_video_filter(f->ctx);
		return;
	}

	const bool srgb = gs_framebuffer_srgb_enabled();
	gs_enable_framebuffer_srgb(false);

	// render can run more than once a frame (preview + projectors): only step once
	if (!f->rendered) {
		f->rendered = true;
		gs_blend_state_push();
		gs_enable_blending(true);
		gs_blend_function(GS_BLEND_ONE, GS_BLEND_ZERO);
		pc_step(f, w, h);
		gs_blend_state_pop();
	}
	if (!f->have_trail) {
		gs_enable_framebuffer_srgb(srgb);
		return;
	}

	set_tex(f, "image", f->cur);
	set_tex(f, "prev_tex", f->acc[0]);
	set_tex(f, "aux_tex", f->glw[0]);
	set_float(f, "glow", f->glow);
	set_float(f, "bgmix", f->bgmix);
	set_float(f, "transparent", f->transparent ? 1.0f : 0.0f);
	while (gs_effect_loop(f->effect, "Out"))
		gs_draw_sprite(NULL, 0, w, h);

	gs_enable_framebuffer_srgb(srgb);
}

struct obs_source_info pointcloud_filter = {
	.id = "pointcloud_filter",
	.type = OBS_SOURCE_TYPE_FILTER,
	.output_flags = OBS_SOURCE_VIDEO,
	.get_name = pc_name,
	.create = pc_create,
	.destroy = pc_destroy,
	.update = pc_update,
	.get_defaults = pc_defaults,
	.get_properties = pc_properties,
	.video_tick = pc_tick,
	.video_render = pc_render,
};
