// LBP cascade face finder (the method of OpenCV's CascadeClassifier with lbpcascade_frontalface): an integral image,
// a 24x24 window slid over the picture at growing sizes, each window put through 20 stages of LBP stumps, then the
// windows that fired grouped (a real face fires many neighbouring windows, a false one only a few).

#include "facedetect.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "face-cascade.h" // FD_FEAT, FD_STAGE, FD_WEAK: data/lbpcascade_frontalface_opencv.xml, made by build.sh

#define MAX_HITS 4096
#define MIN_NEIGHBOURS 3

#define NFEAT (sizeof(FD_FEAT) / sizeof(FD_FEAT[0]))

// each feature's 4x4 grid of integral-image corners, as offsets from the window's top-left, for one window size
static void scale_feats(int *off, int iw, float s)
{
	for (size_t f = 0; f < NFEAT; f++) {
		const unsigned char *r = FD_FEAT[f];
		const int fx = (int)(r[0] * s + 0.5f), fy = (int)(r[1] * s + 0.5f);
		int cw = (int)(r[2] * s + 0.5f), ch = (int)(r[3] * s + 0.5f);
		cw = cw < 1 ? 1 : cw;
		ch = ch < 1 ? 1 : ch;
		for (int j = 0; j < 4; j++)
			for (int q = 0; q < 4; q++)
				off[f * 16 + j * 4 + q] = (fy + j * ch) * iw + fx + q * cw;
	}
}

// does the window whose top-left corner is p pass every stage?
static int window(const uint32_t *p, const int *off)
{
	int k = 0;
	for (int st = 0; st < FD_NSTAGE; st++) {
		float sum = 0.0f;
		for (int i = 0; i < FD_STAGE[st].n; i++, k++) {
			const int *o = off + FD_WEAK[k].f * 16;
			uint32_t c[9]; // 3x3 cells; the code is which outer cells are at least as bright as the middle (OpenCV's bit order)
			for (int j = 0; j < 3; j++)
				for (int q = 0; q < 3; q++)
					c[j * 3 + q] = p[o[(j + 1) * 4 + q + 1]] - p[o[(j + 1) * 4 + q]] - p[o[j * 4 + q + 1]] + p[o[j * 4 + q]];
			const uint32_t m = c[4];
			const unsigned code = (c[0] >= m ? 128u : 0u) | (c[1] >= m ? 64u : 0u) | (c[2] >= m ? 32u : 0u) |
					      (c[5] >= m ? 16u : 0u) | (c[8] >= m ? 8u : 0u) | (c[7] >= m ? 4u : 0u) |
					      (c[6] >= m ? 2u : 0u) | (c[3] >= m ? 1u : 0u);
			sum += FD_WEAK[k].leaf[((uint32_t)FD_WEAK[k].sub[code >> 5] >> (code & 31)) & 1u ? 0 : 1];
		}
		if (sum < FD_STAGE[st].thr)
			return 0;
	}
	return 1;
}

static int same(const struct fd_rect *a, const struct fd_rect *b) // OpenCV's SimilarRects, eps 0.2
{
	const float d = 0.2f * (fminf(a->w, b->w) + fminf(a->h, b->h)) * 0.5f;
	return fabsf(a->x - b->x) <= d && fabsf(a->y - b->y) <= d && fabsf(a->x + a->w - b->x - b->w) <= d &&
	       fabsf(a->y + a->h - b->y - b->h) <= d;
}

static int root(int *p, int i)
{
	while (p[i] != i)
		i = p[i] = p[p[i]];
	return i;
}

static int by_size(const void *a, const void *b)
{
	const float d = ((const struct fd_rect *)b)->w - ((const struct fd_rect *)a)->w;
	return d > 0 ? 1 : d < 0 ? -1 : 0;
}

int fd_detect(const uint8_t *g, int w, int h, int stride, int min_size, struct fd_rect *out, int max_out)
{
	if (w < FD_W || h < FD_H || max_out <= 0)
		return 0;
	const int iw = w + 1;
	uint32_t *ii = calloc((size_t)iw * (h + 1), sizeof(uint32_t));
	struct fd_rect *hit = malloc(sizeof(*hit) * MAX_HITS);
	int *par = malloc(sizeof(int) * MAX_HITS);
	if (!ii || !hit || !par) {
		free(ii), free(hit), free(par);
		return 0;
	}
	for (int y = 0; y < h; y++) {
		uint32_t run = 0;
		for (int x = 0; x < w; x++) {
			run += g[y * stride + x];
			ii[(y + 1) * iw + x + 1] = ii[y * iw + x + 1] + run;
		}
	}

	int nh = 0, off[NFEAT * 16];
	for (float s = fmaxf(1.0f, (float)min_size / FD_W); FD_W * s <= w && FD_H * s <= h; s *= 1.1f) {
		const int ww = (int)(FD_W * s + 0.5f), wh = (int)(FD_H * s + 0.5f);
		const int step = s <= 2.0f ? (int)(2.0f * s + 0.5f) : (int)(s + 0.5f); // OpenCV: 2 px of the shrunk picture below 2x, else 1
		scale_feats(off, iw, s);
		for (int y = 0; y + wh < h; y += step)
			for (int x = 0; x + ww < w && nh < MAX_HITS; x += step)
				if (window(ii + y * iw + x, off))
					hit[nh++] = (struct fd_rect){(float)x, (float)y, (float)ww, (float)wh, 1};
	}

	// group the hits; a group needs MIN_NEIGHBOURS windows to count
	for (int i = 0; i < nh; i++)
		par[i] = i;
	for (int i = 0; i < nh; i++)
		for (int j = i + 1; j < nh; j++)
			if (same(&hit[i], &hit[j]))
				par[root(par, i)] = root(par, j);
	struct fd_rect grp[64];
	int ng = 0;
	for (int i = 0; i < nh; i++) {
		if (root(par, i) != i)
			continue;
		struct fd_rect a = {0};
		for (int j = 0; j < nh; j++)
			if (root(par, j) == i) {
				a.x += hit[j].x, a.y += hit[j].y, a.w += hit[j].w, a.h += hit[j].h;
				a.n++;
			}
		if (a.n < MIN_NEIGHBOURS || ng == 64)
			continue;
		a.x /= a.n, a.y /= a.n, a.w /= a.n, a.h /= a.n;
		grp[ng++] = a;
	}
	free(ii), free(hit), free(par);

	// drop a small group sitting inside a bigger, surer one
	qsort(grp, ng, sizeof(grp[0]), by_size);
	int n = 0;
	for (int i = 0; i < ng && n < max_out; i++) {
		int inside = 0;
		for (int j = 0; j < n && !inside; j++) {
			const struct fd_rect *b = &out[j];
			const float dx = b->w * 0.2f, dy = b->h * 0.2f;
			inside = grp[i].x >= b->x - dx && grp[i].y >= b->y - dy && grp[i].x + grp[i].w <= b->x + b->w + dx &&
				 grp[i].y + grp[i].h <= b->y + b->h + dy && (b->n > 3 || b->n >= grp[i].n);
		}
		if (!inside)
			out[n++] = grp[i];
	}
	return n;
}
