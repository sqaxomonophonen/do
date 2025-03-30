#include <stdlib.h>
#include <stddef.h>
#include <stdint.h>
#include <assert.h>
#include <math.h>

#include "sep2dconv.h"

static int f32_scratch_capacity_log2;
static float* f32_scratch;

void sep2dconv_execute(struct sep2dconv_kernel* kernel, uint8_t* image, int width, int height, int stride)
{
	const int num_pixels = width*height;

	int do_allocate = (f32_scratch == NULL);
	while ((1<<f32_scratch_capacity_log2) < num_pixels) {
		do_allocate = 1;
		f32_scratch_capacity_log2++;
	}
	if (do_allocate) {
		f32_scratch = realloc(f32_scratch, sizeof(*f32_scratch)<<f32_scratch_capacity_log2);
		assert(f32_scratch != NULL);
	}

	const int R = kernel->radius;
	const int R2 = 2*R;
	const int R21 = R2+1;
	const float* K = kernel->coefficients;
	const float* Kend = K+R21;

	float* const scratch_end = f32_scratch+num_pixels;

	// first pass; X-axis convolution; result is written to scratch with
	// x/y axes swapped (meaning the 2nd pass Y-convolution can read from
	// scratch in X-direction)
	const int y0 = R;
	const int y1 = height-R;
	const int dy = y1-y0;
	const int scratch_stride = dy;
	int pyoff = stride*y0;
	for (int yi = 0; yi < dy; ++yi, pyoff+=stride) {
		float* sp = f32_scratch + yi;
		const uint8_t* pb = image + pyoff;
		const uint8_t* pbend = pb + stride;
		for (int x = 0; x < width; ++x, sp+=scratch_stride) {
			const int k0 = ((x<R) ? (R-x) : 0);
			const float* k = K + k0;
			const int km = R21-k0;
			const int p0 = ((x<R) ? 0 : (x-R));
			const uint8_t* p = pb + p0;
			const int nm = width-p0;
			const int n = km<nm ? km : nm;
			float sum = 0.0f;
			for (int i=0; i<n; ++i, ++p, ++k) {
				assert(pb <= p && p < pbend);
				assert(K <= k && k < Kend);
				sum += (float)*p * (1.0f/255.0f) * (*k);
			}
			assert(f32_scratch <= sp && sp < scratch_end);
			(*sp) = sum;
		}
	}

	// second pass; Y-axis convolution
	for (int x = 0; x < width; ++x) {
		const float* const spb = f32_scratch + x*scratch_stride;
		const float* const spb_end = spb+scratch_stride;
		assert(f32_scratch <= spb && spb_end <= scratch_end);
		uint8_t* p = image + x;
		for (int y = 0; y < height; ++y, p+=stride) {
			const int k0 = ((y<R2) ? (R2-y) : 0);
			const float* k = K + k0;
			const int km = R21-k0;
			const int s0 = ((y<R2) ? 0 : (y-R2));
			const float* const spb2 = spb + s0;
			const int nm = height-R2-s0;
			assert(nm > 0);
			const int n = km<nm ? km : nm;
			float sum = 0.0f;
			const float* sp = spb2;
			for (int i=0; i<n; ++i, ++sp, ++k) {
				assert(spb <= sp && sp < spb_end);
				assert(K <= k && k < Kend);
				sum += *(sp) * (*k);
			}
			int v = floorf(sum*256.0f);
			if (v<0) v=0;
			if (v>255) v=255;
			(*p) = v;
		}
	}
}
