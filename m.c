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

float bezier1(float t, float x0, float x1, float x2, float x3)
{
	float t1 = 1-t;
	return t1*t1*t1*x0 + 3*t*t1*t1*x1 + 3*t*t*t1*x2 + t*t*t*x3;
}

union vec2 bezier2(float t, union vec2 p0, union vec2 p1, union vec2 p2, union vec2 p3)
{
	return (union vec2) {
		.x = bezier1(t, p0.x, p1.x, p2.x, p3.x),
		.y = bezier1(t, p0.y, p1.y, p2.y, p3.y)
	};
}

float point_lineseg_distance(union vec2 o, union vec2 p0, union vec2 p1)
{
	union vec2 d = vec2_sub(p1, p0);
	float d2 = vec2_dot(d, d);
	if (d2 == 0.0) return vec2_length(vec2_sub(o, p0));
	float t = fmax(0.0, fmin(1.0, vec2_dot(vec2_sub(o, p0), vec2_sub(p1, p0)) / d2));
	union vec2 proj = vec2_add(p0, vec2_scale(vec2_sub(p1, p0), t));
	return vec2_length(vec2_sub(o, proj));

}

float bezier2_distance(union vec2 o, union vec2 p0, union vec2 p1, union vec2 p2, union vec2 p3)
{
	const int N = 25;

	union vec2 q0 = bezier2(0.0, p0, p1, p2, p3);
	float distance = 0;
	for (int i = 1; i <= N; i++) {
		union vec2 q1 = bezier2((float)i / (float)N, p0, p1, p2, p3);
		float d = point_lineseg_distance(o, q0, q1);
		if (i == 1 || d < distance) distance = d;
		q0 = q1;
	}
	return distance;
}
