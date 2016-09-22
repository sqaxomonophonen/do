#include <math.h>

#include "m.h"

union vec2 vec2_add(union vec2 a, union vec2 b)
{
	union vec2 r;
	for (int i = 0; i < 2; i++) r.s[i] = a.s[i] + b.s[i];
	return r;
}

union vec2 vec2_sub(union vec2 a, union vec2 b)
{
	union vec2 r;
	for (int i = 0; i < 2; i++) r.s[i] = a.s[i] - b.s[i];
	return r;
}

float vec2_dot(union vec2 a, union vec2 b)
{
	float product = 0;
	for (int i = 0; i < 2; i++) product += a.s[i] * b.s[i];
	return product;
}

float vec2_length(union vec2 v)
{
	return sqrtf(vec2_dot(v, v));
}

union vec2 vec2_scale(union vec2 v, float s)
{
	for (int i = 0; i < 2; i++) v.s[i] *= s;
	return v;
}

union vec2 vec2_unit(union vec2 v)
{
	return vec2_scale(v, 1/vec2_length(v));
}

union vec2 vec2_normal(union vec2 v)
{
	return (union vec2) {
		.x = -v.y,
		.y = v.x
	};
}

int rect_not_empty(struct rect* r)
{
	return r->dim.w > 0 && r->dim.h > 0;
}

int rect_contains_point(struct rect* rect, union vec2 point)
{
	int inside = 1;
	for (int axis = 0; axis < 2; axis++) {
		inside &=
			point.s[axis] >= rect->p0.s[axis]
			&& point.s[axis] < (rect->p0.s[axis] + rect->dim.s[axis]);
	}
	return inside;
}

void rect_split_vertical(struct rect* r, float height, struct rect* a, struct rect* b)
{
	struct rect cp = *r;
	if (a) *a = (struct rect) { .p0 = cp.p0, .dim = { .w = cp.dim.w, .h = height }};
	if (b) *b = (struct rect) { .p0 = { .x = cp.p0.x, .y = cp.p0.y + height }, .dim = { .w = cp.dim.w, .h = cp.dim.h - height }};
}

void rect_split_horizontal(struct rect* r, float width, struct rect* a, struct rect* b)
{
	struct rect cp = *r;
	if (a) *a = (struct rect) { .p0 = cp.p0, .dim = { .w = width, .h = cp.dim.h }};
	if (b) *b = (struct rect) { .p0 = { .x = cp.p0.x + width, .y = cp.p0.y }, .dim = { .w = cp.dim.w - width, .h = cp.dim.h }};
}

