#ifndef M_H

/*
m.h: math structures and functions
*/

union vec2 {
	float s[2];
	struct { float x, y; };
	struct { float u, v; };
	struct { float w, h; };
};

union vec4 {
	float s[4];
	struct { float x, y, z, w; };
	struct { float r, g, b, a; };
};

struct rect {
	union vec2 p0, dim;
};

union vec2 vec2_add(union vec2 a, union vec2 b);
union vec2 vec2_sub(union vec2 a, union vec2 b);
float vec2_dot(union vec2 a, union vec2 b);
float vec2_length(union vec2 v);
union vec2 vec2_scale(union vec2 v, float s);
union vec2 vec2_unit(union vec2 v);
union vec2 vec2_normal(union vec2 v);

struct rect rect_from_points(union vec2 p0, union vec2 p1);
int rect_not_empty(struct rect* r);
int rect_contains_point(struct rect* rect, union vec2 point);
int rect_overlaps(struct rect* rect0, struct rect* rect1);
void rect_split_vertical(struct rect* r, float height, struct rect* a, struct rect* b);
void rect_split_horizontal(struct rect* r, float width, struct rect* a, struct rect* b);

float point_lineseg_distance(union vec2 o, union vec2 p0, union vec2 p1);

float bezier1(float t, float x0, float x1, float x2, float x3);
union vec2 bezier2(float t, union vec2 p0, union vec2 p1, union vec2 p2, union vec2 p3);
float bezier2_distance(union vec2 o, union vec2 p0, union vec2 p1, union vec2 p2, union vec2 p3);

#define M_H
#endif
