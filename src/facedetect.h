// Frontal face finder: OpenCV's LBP face cascade run on a small grey picture on the CPU. No model files, no libraries.
#pragma once
#include <stdint.h>

struct fd_rect {
	float x, y, w, h; // in pixels of the grey picture
	int n;            // how many windows agreed (more = surer)
};

// grey: w x h, one byte per pixel, `stride` bytes per row. Faces smaller than min_size pixels are skipped.
// Returns how many faces went into out (largest first, at most max_out).
int fd_detect(const uint8_t *grey, int w, int h, int stride, int min_size, struct fd_rect *out, int max_out);
