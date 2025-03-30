#ifndef SEP2DCONV // separable 2d convolution

struct sep2dconv_kernel {
	int radius;
	float* coefficients;
	// the `coefficients` array must have `2*radius+1` elements.
};

void sep2dconv_execute(struct sep2dconv_kernel* kernel, uint8_t* image, int width, int height, int stride);
// XXX not thread-safe (static vars at the top of sep2dconv.c needs to be
// handled thread-locally)

#define SEP2DCONV
#endif
