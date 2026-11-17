// Face tracking shared by the Face Censor and Face Sticker filters. Each frame a small grey copy of the picture is read
// back a frame late and handed to a worker thread running the face finder (facedetect.c); its results are matched to up
// to FT_MAX tracks, smoothed with moshcap's util.follow (STEADY) and held for a moment when the finder misses a face.
#pragma once
#include <obs-module.h>

#define FT_MAX 4

struct ft_track {
	bool on;
	bool seen;     // matched on the finder's latest look (else held)
	float box[4];  // x0, y0, x1, y1 in source pixels (smoothed)
	float in[4], v[4];
	float lost;    // seconds since the finder last saw it
};

struct ft_priv;

struct facetrack {
	struct ft_track tr[FT_MAX];
	uint32_t w, h; // source size the boxes are in
	struct ft_priv *p;
};

// both need the graphics context NOT entered (they enter it themselves)
struct facetrack *ft_create(void);
void ft_destroy(struct facetrack *ft);

// once per frame from video_tick: ages the tracks, drops those unseen for longer than hold seconds
void ft_tick(struct facetrack *ft, float dt, float hold);

// once per frame inside video_render: grey copy of src (w x h), read back last frame's, take any new results
void ft_step(struct facetrack *ft, gs_texture_t *src, uint32_t w, uint32_t h, bool small_faces, float smooth);

bool ft_running(const struct facetrack *ft);

// debug: "finder 320x180 thread=ok 29.0 looks/s 5.0 ms/look, last look found 1 | tracks: ..." (resets the counters)
void ft_status(struct facetrack *ft, float seconds, char *out, size_t size);
