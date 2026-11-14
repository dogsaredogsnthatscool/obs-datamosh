// Headless test for the filters: plays raw RGBA frames through a filter inside libobs (no OBS UI) and dumps
// chosen output frames as raw RGBA. See test/run.sh.
//   harness plugin.so filter_id frames.rgba W H '{"json":"settings"}' outprefix frame,frame,...
#include <obs.h>
#include <obs-nix-platform.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

void *XOpenDisplay(const char *);

static uint8_t *frames;
static int nframes, W, H, tick_n;
static const char *outprefix;
static int want[64], nwant, got;

struct tsrc {
	gs_texture_t *tex;
};

static const char *ts_name(void *u)
{
	return "test frames";
}
static void *ts_create(obs_data_t *s, obs_source_t *src)
{
	return bzalloc(sizeof(struct tsrc));
}
static void ts_destroy(void *d)
{
	struct tsrc *t = d;
	obs_enter_graphics();
	gs_texture_destroy(t->tex);
	obs_leave_graphics();
	bfree(t);
}
static uint32_t ts_w(void *d)
{
	return W;
}
static uint32_t ts_h(void *d)
{
	return H;
}
static void ts_tick(void *d, float s)
{
	tick_n++;
}
static void ts_render(void *d, gs_effect_t *e)
{
	struct tsrc *t = d;
	if (!t->tex)
		t->tex = gs_texture_create(W, H, GS_RGBA, 1, NULL, GS_DYNAMIC);
	gs_texture_set_image(t->tex, frames + (size_t)(tick_n % nframes) * W * H * 4, W * 4, false);
	gs_effect_t *def = obs_get_base_effect(OBS_EFFECT_DEFAULT);
	gs_effect_set_texture(gs_effect_get_param_by_name(def, "image"), t->tex);
	while (gs_effect_loop(def, "Draw"))
		gs_draw_sprite(t->tex, 0, W, H);
}

static struct obs_source_info test_src = {
	.id = "test_frames",
	.type = OBS_SOURCE_TYPE_INPUT,
	.output_flags = OBS_SOURCE_VIDEO | OBS_SOURCE_CUSTOM_DRAW,
	.get_name = ts_name,
	.create = ts_create,
	.destroy = ts_destroy,
	.get_width = ts_w,
	.get_height = ts_h,
	.video_tick = ts_tick,
	.video_render = ts_render,
};

static int out_n;
static void raw_cb(void *param, struct video_data *f)
{
	int n = out_n++;
	for (int i = 0; i < nwant; i++) {
		if (want[i] != n)
			continue;
		char path[512];
		snprintf(path, sizeof(path), "%s-%04d.rgba", outprefix, n);
		FILE *o = fopen(path, "wb");
		for (int y = 0; y < H; y++)
			fwrite(f->data[0] + (size_t)y * f->linesize[0], 1, (size_t)W * 4, o);
		fclose(o);
		got++;
	}
}

int main(int argc, char **argv)
{
	if (argc < 9)
		return 2;
	W = atoi(argv[4]);
	H = atoi(argv[5]);
	outprefix = argv[7];
	for (char *p = strtok(argv[8], ","); p && nwant < 64; p = strtok(NULL, ","))
		want[nwant++] = atoi(p);

	FILE *in = fopen(argv[3], "rb");
	fseek(in, 0, SEEK_END);
	long sz = ftell(in);
	fseek(in, 0, SEEK_SET);
	frames = malloc(sz);
	if (fread(frames, 1, sz, in) != (size_t)sz)
		return 3;
	nframes = (int)(sz / ((long)W * H * 4));

	obs_set_nix_platform(OBS_NIX_PLATFORM_X11_EGL);
	obs_set_nix_platform_display(XOpenDisplay(NULL));
	if (getenv("OBS_DATA")) // .../share/obs/libobs/ when OBS is not installed system-wide
		obs_add_data_path(getenv("OBS_DATA"));
	if (!obs_startup("en-US", NULL, NULL))
		return 4;
	struct obs_video_info ovi = {
		.graphics_module = "libobs-opengl",
		.fps_num = 30,
		.fps_den = 1,
		.base_width = W,
		.base_height = H,
		.output_width = W,
		.output_height = H,
		.output_format = VIDEO_FORMAT_RGBA,
		.colorspace = VIDEO_CS_SRGB,
		.range = VIDEO_RANGE_FULL,
		.scale_type = OBS_SCALE_BILINEAR,
	};
	int r = obs_reset_video(&ovi);
	if (r != OBS_VIDEO_SUCCESS) {
		fprintf(stderr, "obs_reset_video: %d\n", r);
		return 5;
	}
	struct obs_audio_info oai = {.samples_per_sec = 48000, .speakers = SPEAKERS_STEREO};
	obs_reset_audio(&oai);

	obs_module_t *mod;
	if (obs_open_module(&mod, argv[1], NULL) != MODULE_SUCCESS || !obs_init_module(mod))
		return 6;
	obs_register_source(&test_src);

	obs_source_t *src = obs_source_create("test_frames", "src", NULL, NULL);
	obs_data_t *set = obs_data_create_from_json(argv[6]);
	obs_source_t *flt = obs_source_create(argv[2], "flt", set, NULL);
	if (!src || !flt)
		return 7;
	obs_source_filter_add(src, flt);
	obs_set_output_source(0, src);
	obs_add_raw_video_callback(NULL, raw_cb, NULL);

	// LEARN=n: press the filter's "learn" button once n frames have played (clip must be an empty room until then)
	int learn = getenv("LEARN") ? atoi(getenv("LEARN")) : -1;
	for (int i = 0; i < 2000 && got < nwant; i++) {
		if (learn >= 0 && tick_n >= learn) {
			obs_properties_t *pr = obs_source_properties(flt);
			obs_property_button_clicked(obs_properties_get(pr, "learn"), flt);
			obs_properties_destroy(pr);
			learn = -1;
		}
		usleep(10000);
	}
	obs_remove_raw_video_callback(raw_cb, NULL);
	obs_set_output_source(0, NULL);
	obs_source_release(flt);
	obs_source_release(src);
	obs_data_release(set);
	obs_shutdown();
	fprintf(stderr, "frames out: %d, saved: %d\n", out_n, got);
	return got == nwant ? 0 : 1;
}
