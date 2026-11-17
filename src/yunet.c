// YuNet in plain C: the network from data/face_detection_yunet_2023mar.onnx, turned into an op list by
// data/onnx2yunet.py, run on the CPU, then decoded the way OpenCV's FaceDetectorYN does it (face_detect.cpp).
// The input is BGR 0..255 (no scaling), padded on the right / bottom to a multiple of 32.

#include "yunet.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "yunet-model.h" // YN_OPS, YN_HEAD, YN_W: made by build.sh

struct yunet {
	int pw, ph;          // padded input size the buffers are made for
	int c[YN_NT], h[YN_NT], w[YN_NT];
	float *t[YN_NT];
};

struct yunet *yn_create(void)
{
	return calloc(1, sizeof(struct yunet));
}

static void free_bufs(struct yunet *yn)
{
	for (int i = 0; i < YN_NT; i++) {
		free(yn->t[i]);
		yn->t[i] = NULL;
	}
}

void yn_destroy(struct yunet *yn)
{
	if (!yn)
		return;
	free_bufs(yn);
	free(yn);
}

// the size of every tensor for a pw x ph input, and a buffer for each
static int plan(struct yunet *yn, int pw, int ph)
{
	if (yn->pw == pw && yn->ph == ph)
		return 1;
	free_bufs(yn);
	memset(yn->c, 0, sizeof(yn->c));
	yn->c[0] = 3;
	yn->h[0] = ph;
	yn->w[0] = pw;
	for (int i = 0; i < YN_NOPS; i++) {
		const struct yn_op *o = &YN_OPS[i];
		int c = yn->c[o->a], h = yn->h[o->a], w = yn->w[o->a];
		switch (o->op) {
		case YN_CONV:
			h = (h + 2 * o->pad - o->k) / o->stride + 1;
			w = (w + 2 * o->pad - o->k) / o->stride + 1;
			c = o->cout;
			break;
		case YN_POOL:
			h /= 2;
			w /= 2;
			break;
		case YN_UP2:
			h *= 2;
			w *= 2;
			break;
		}
		yn->c[o->out] = c;
		yn->h[o->out] = h;
		yn->w[o->out] = w;
	}
	for (int i = 0; i < YN_NT; i++)
		if (yn->c[i]) {
			yn->t[i] = malloc(sizeof(float) * yn->c[i] * yn->h[i] * yn->w[i]);
			if (!yn->t[i]) {
				free_bufs(yn);
				yn->pw = yn->ph = 0;
				return 0;
			}
		}
	yn->pw = pw;
	yn->ph = ph;
	return 1;
}

static void conv(const struct yn_op *o, const float *in, int H, int W, float *out, int OH, int OW)
{
	const float *wt = YN_W + o->w, *bias = YN_W + o->bias;
	const int cin = o->cin, cout = o->cout, k = o->k, s = o->stride, pad = o->pad, g = o->group;
	const int ohw = OH * OW, ihw = H * W;
	if (k == 1 && g == 1 && s == 1) { // pointwise: a small matrix product, the inner loop runs along the picture
		for (int co = 0; co < cout; co++) {
			float *dst = out + co * ohw;
			const float b = bias[co];
			for (int p = 0; p < ohw; p++)
				dst[p] = b;
			for (int ci = 0; ci < cin; ci++) {
				const float wv = wt[co * cin + ci];
				const float *src = in + ci * ihw;
				for (int p = 0; p < ohw; p++)
					dst[p] += wv * src[p];
			}
		}
	} else if (k == 3 && g == cin && g == cout && s == 1 && pad == 1) { // depthwise 3x3
		for (int c = 0; c < cout; c++) {
			const float *src = in + c * ihw, *kw = wt + c * 9;
			float *dst = out + c * ohw;
			const float b = bias[c];
			for (int y = 0; y < OH; y++) {
				for (int x = 0; x < OW; x++) {
					float acc = b;
					if (y > 0 && y < H - 1 && x > 0 && x < W - 1) {
						const float *r0 = src + (y - 1) * W + x - 1, *r1 = r0 + W, *r2 = r1 + W;
						acc += kw[0] * r0[0] + kw[1] * r0[1] + kw[2] * r0[2] + kw[3] * r1[0] + kw[4] * r1[1] + kw[5] * r1[2] +
						       kw[6] * r2[0] + kw[7] * r2[1] + kw[8] * r2[2];
					} else {
						for (int ky = 0; ky < 3; ky++) {
							const int iy = y + ky - 1;
							if (iy < 0 || iy >= H)
								continue;
							for (int kx = 0; kx < 3; kx++) {
								const int ix = x + kx - 1;
								if (ix >= 0 && ix < W)
									acc += kw[ky * 3 + kx] * src[iy * W + ix];
							}
						}
					}
					dst[y * OW + x] = acc;
				}
			}
		}
	} else if (k == 3 && pad == 1) { // any other 3x3 (the first stride-2 conv, the small head convs): one weight at a time over whole rows
		const int cig = cin / g, cog = cout / g;
		for (int co = 0; co < cout; co++) {
			float *dst = out + co * ohw;
			for (int p = 0; p < ohw; p++)
				dst[p] = bias[co];
			const int gi = co / cog;
			for (int ci = 0; ci < cig; ci++) {
				const float *src = in + (gi * cig + ci) * ihw, *kw = wt + (co * cig + ci) * 9;
				for (int ky = 0; ky < 3; ky++)
					for (int kx = 0; kx < 3; kx++) {
						const float wv = kw[ky * 3 + kx];
						int x0 = 0, x1 = OW; // output columns whose input column x * s + kx - 1 is inside
						while (x0 < OW && x0 * s + kx - 1 < 0)
							x0++;
						while (x1 > x0 && (x1 - 1) * s + kx - 1 >= W)
							x1--;
						for (int y = 0; y < OH; y++) {
							const int iy = y * s + ky - 1;
							if (iy < 0 || iy >= H)
								continue;
							const float *row = src + iy * W + kx - 1;
							float *d = dst + y * OW;
							if (s == 1)
								for (int x = x0; x < x1; x++)
									d[x] += wv * row[x];
							else
								for (int x = x0; x < x1; x++)
									d[x] += wv * row[x * s];
						}
					}
			}
		}
	} else { // anything else: direct
		const int cig = cin / g, cog = cout / g;
		for (int co = 0; co < cout; co++) {
			const int gi = co / cog;
			float *dst = out + co * ohw;
			for (int y = 0; y < OH; y++)
				for (int x = 0; x < OW; x++) {
					float acc = bias[co];
					for (int ci = 0; ci < cig; ci++) {
						const float *src = in + (gi * cig + ci) * ihw, *kw = wt + (co * cig + ci) * k * k;
						for (int ky = 0; ky < k; ky++) {
							const int iy = y * s + ky - pad;
							if (iy < 0 || iy >= H)
								continue;
							for (int kx = 0; kx < k; kx++) {
								const int ix = x * s + kx - pad;
								if (ix >= 0 && ix < W)
									acc += kw[ky * k + kx] * src[iy * W + ix];
							}
						}
					}
					dst[y * OW + x] = acc;
				}
		}
	}
	if (o->relu)
		for (int i = 0; i < cout * ohw; i++)
			out[i] = out[i] > 0.0f ? out[i] : 0.0f;
}

static void run(struct yunet *yn)
{
	for (int i = 0; i < YN_NOPS; i++) {
		const struct yn_op *o = &YN_OPS[i];
		const float *a = yn->t[o->a];
		float *d = yn->t[o->out];
		const int C = yn->c[o->a], H = yn->h[o->a], W = yn->w[o->a], OH = yn->h[o->out], OW = yn->w[o->out];
		switch (o->op) {
		case YN_CONV:
			conv(o, a, H, W, d, OH, OW);
			break;
		case YN_POOL:
			for (int c = 0; c < C; c++)
				for (int y = 0; y < OH; y++)
					for (int x = 0; x < OW; x++) {
						const float *p = a + (c * H + y * 2) * W + x * 2;
						float m = p[0] > p[1] ? p[0] : p[1];
						m = m > p[W] ? m : p[W];
						m = m > p[W + 1] ? m : p[W + 1];
						d[(c * OH + y) * OW + x] = m;
					}
			break;
		case YN_UP2:
			for (int c = 0; c < C; c++)
				for (int y = 0; y < OH; y++)
					for (int x = 0; x < OW; x++)
						d[(c * OH + y) * OW + x] = a[(c * H + y / 2) * W + x / 2];
			break;
		case YN_ADD: {
			const float *b = yn->t[o->b];
			for (int i2 = 0; i2 < C * H * W; i2++)
				d[i2] = a[i2] + b[i2];
			break;
		}
		}
	}
}

static float sigmoid(float v)
{
	return 1.0f / (1.0f + expf(-v));
}

static float iou(const struct yn_face *a, const struct yn_face *b)
{
	const float x0 = fmaxf(a->x, b->x), y0 = fmaxf(a->y, b->y);
	const float x1 = fminf(a->x + a->w, b->x + b->w), y1 = fminf(a->y + a->h, b->y + b->h);
	const float in = fmaxf(0.0f, x1 - x0) * fmaxf(0.0f, y1 - y0);
	return in / fmaxf(1e-6f, a->w * a->h + b->w * b->h - in);
}

static int by_score(const void *a, const void *b)
{
	const float d = ((const struct yn_face *)b)->score - ((const struct yn_face *)a)->score;
	return d > 0 ? 1 : d < 0 ? -1 : 0;
}

#define MAX_CAND 256

int yn_detect(struct yunet *yn, const uint8_t *rgba, int w, int h, int stride, float thresh, struct yn_face *out, int max_out)
{
	if (!yn || w < 32 || h < 32 || max_out <= 0)
		return 0;
	const int pw = (w + 31) / 32 * 32, ph = (h + 31) / 32 * 32;
	if (!plan(yn, pw, ph))
		return 0;
	float *in = yn->t[0]; // planar B, G, R; padding stays 0
	memset(in, 0, sizeof(float) * 3 * pw * ph);
	for (int y = 0; y < h; y++) {
		const uint8_t *p = rgba + y * stride;
		for (int x = 0; x < w; x++, p += 4) {
			in[(0 * ph + y) * pw + x] = p[2];
			in[(1 * ph + y) * pw + x] = p[1];
			in[(2 * ph + y) * pw + x] = p[0];
		}
	}
	run(yn);

	struct yn_face cand[MAX_CAND];
	int nc = 0;
	static const int strides[3] = {8, 16, 32};
	for (int si = 0; si < 3; si++) {
		const int st = strides[si], cols = pw / st, rows = ph / st, n = cols * rows;
		const float *cls = yn->t[YN_HEAD[0][si]], *obj = yn->t[YN_HEAD[1][si]];
		const float *bb = yn->t[YN_HEAD[2][si]], *kp = yn->t[YN_HEAD[3][si]]; // planar: channel k at [k * n + idx]
		for (int r = 0; r < rows; r++)
			for (int c = 0; c < cols; c++) {
				const int i = r * cols + c;
				const float sc = sqrtf(sigmoid(cls[i]) * sigmoid(obj[i]));
				if (sc < thresh)
					continue;
				struct yn_face f;
				const float cx = (c + bb[0 * n + i]) * st, cy = (r + bb[1 * n + i]) * st;
				f.w = expf(bb[2 * n + i]) * st;
				f.h = expf(bb[3 * n + i]) * st;
				f.x = cx - f.w / 2;
				f.y = cy - f.h / 2;
				f.score = sc;
				for (int k = 0; k < 5; k++) {
					f.lm[2 * k] = (kp[(2 * k) * n + i] + c) * st;
					f.lm[2 * k + 1] = (kp[(2 * k + 1) * n + i] + r) * st;
				}
				if (nc < MAX_CAND)
					cand[nc++] = f;
			}
	}
	qsort(cand, nc, sizeof(cand[0]), by_score);
	int no = 0;
	for (int i = 0; i < nc && no < max_out; i++) {
		int keep = 1;
		for (int j = 0; j < no && keep; j++)
			keep = iou(&cand[i], &out[j]) <= 0.3f;
		if (keep)
			out[no++] = cand[i];
	}
	return no;
}
