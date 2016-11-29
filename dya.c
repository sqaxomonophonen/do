#include <assert.h>
#include <string.h>
#include <stdlib.h>

#include "dya.h"

const int min_cap_log2 = 10;
const int min_ncap = 16;

static void resize(struct dya* d, void** ptr)
{
	u8 old_cap_log2 = d->cap_log2;
	int ncap = d->n > min_ncap ? d->n : min_ncap;
	while ((1<<d->cap_log2) < ncap*d->element_sz) d->cap_log2++;
	if (d->cap_log2 < d->min_cap_log2) d->cap_log2 = d->min_cap_log2;
	if (d->cap_log2 != old_cap_log2) *ptr = realloc(*ptr, 1<<d->cap_log2);
	assert(*ptr != NULL);
}

void dya_init(struct dya* d, void** ptr, size_t element_sz)
{
	assert(d != NULL);
	assert(element_sz < (1<<16));
	memset(d, 0, sizeof *d);
	*ptr = NULL;
	d->element_sz = element_sz;
	dya_set_min_cap(d, 1<<min_cap_log2);
}

void dya_set_min_cap(struct dya* d, int min_cap)
{
	int min_cap_log2 = 0;
	while ((1 << min_cap_log2) < min_cap) min_cap_log2++;
	d->min_cap_log2 = min_cap_log2;
}

void dya_clone(struct dya* dst, void** dstptr, struct dya* src, void** srcptr)
{
	*dstptr = NULL;
	memcpy(dst, src, sizeof *src);
	resize(dst, dstptr);
	memcpy(*dstptr, *srcptr, src->n * src->element_sz);
}

void dya_clear(struct dya* d, void** ptr)
{
	d->n = 0;
	resize(d, ptr);
}

void* dya_append(struct dya* d, void** ptr, void* new_element)
{
	d->n++;
	resize(d, ptr);
	void* dst = *ptr + d->element_sz * (d->n-1);
	memcpy(dst, new_element, d->element_sz);
	return dst;
}

void dya_delete(struct dya* d, void** ptr, int index)
{
	assert(index >= 0);
	assert(index < d->n);
	int to_move = d->n - 1 - index;
	if (to_move > 0) {
		memmove(
			*ptr + d->element_sz * index,
			*ptr + d->element_sz * (index+1),
			to_move * d->element_sz);
	}
	d->n--;
	resize(d, ptr);
}

void dya_delete_scan(struct dya* d, void** ptr, int (*match)(const void*, void*), void* usr)
{
	int n_deleted = 0;
	int i1 = 0;
	for (int i0 = 0; i0 < d->n; i0++) {
		void* p = *ptr + i0 * d->element_sz;
		if (match(p, usr)) {
			n_deleted++;
		} else {
			if (i0 != i1) memcpy(*ptr + i1 * d->element_sz, p, d->element_sz);
			i1++;
		}
	}
	if (n_deleted > 0) {
		d->n -= n_deleted;
		resize(d, ptr);
	}
}

void* dya_insert(struct dya* d, void** ptr, int index, void* new_element)
{
	assert(index >= 0);
	assert(index <= d->n);
	int to_move = d->n - index;
	d->n++;
	resize(d, ptr);
	if (to_move > 0) {
		memmove(
			*ptr + d->element_sz * (index+1),
			*ptr + d->element_sz * index,
			to_move * d->element_sz);
	}
	void* dst = *ptr + d->element_sz * index;
	memcpy(dst, new_element, d->element_sz);
	return dst;
}

void dya_qsort(struct dya* d, void** ptr, int (*compar)(const void *, const void *))
{
	qsort(*ptr, d->n, d->element_sz, compar);
}

/* returns index of key, otherwise negative (-retval-1 is the insertion point) */
int dya_bs_find(struct dya* d, void** ptr, int (*compar)(const void *, const void *), void* key)
{
	int l = 0;
	int r = d->n - 1;
	while (l <= r) {
		int m = (l+r) >> 1;
		int cmp = compar(*ptr + d->element_sz * m, key);
		if (cmp < 0) {
			l = m + 1;
		} else if (cmp > 0) {
			r = m - 1;
		} else {
			return m;
		}
	}
	return -(l+1);
}

void* dya_bs_insert(struct dya* d, void** ptr, int (*compar)(const void *, const void *), void* new_element)
{
	int index = dya_bs_find(d, ptr, compar, new_element);
	if (index >= 0) assert(!"duplicate element");
	return dya_insert(d, ptr, -index-1, new_element);
}

void dya_bs_delete(struct dya* d, void** ptr, int (*compar)(const void *, const void *), void* key)
{
	int index = dya_bs_find(d, ptr, compar, key);
	if (index >= 0) dya_delete(d, ptr, index);
}
