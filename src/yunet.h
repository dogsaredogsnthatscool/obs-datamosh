// YuNet face detector (OpenCV Zoo, 2023mar, MIT), run in plain C on the CPU: faces with five landmarks each.
#pragma once
#include <stdint.h>

struct yn_face {
	float x, y, w, h; // box, in pixels of the picture given
	float score;
	float lm[10];     // x, y of: the person's right eye, left eye, nose tip, right mouth corner, left mouth corner
};

struct yunet;

struct yunet *yn_create(void);
void yn_destroy(struct yunet *yn);

// rgba: w x h, 4 bytes per pixel (R, G, B, A), `stride` bytes per row. Faces scoring under `thresh` (0.6 is OpenCV's
// usual) are dropped, overlaps merged (NMS 0.3). Returns how many went into out, best first.
int yn_detect(struct yunet *yn, const uint8_t *rgba, int w, int h, int stride, float thresh, struct yn_face *out, int max_out);
