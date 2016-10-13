#ifndef DYA_H

/*
dya.h: dynamic array, including binary search (_bs_) operations.
*/

#include "dtypes.h"

struct dya {
	u32 n;
	u16 element_sz;
	u8 cap_log2;
	u8 flags;
};

void dya_init(struct dya*, void** ptr, size_t element_sz, uint8_t flags);
void dya_clone(struct dya* dst, void** dstptr, struct dya* src, void** srcptr);
void dya_clear(struct dya*, void** ptr);
void* dya_append(struct dya*, void** ptr, void* new_element);
void dya_delete(struct dya*, void** ptr, int index);
void dya_delete_scan(struct dya*, void** ptr, int (*match)(const void*, void*), void* usr);
void* dya_insert(struct dya*, void** ptr, int index, void* new_element);
void dya_qsort(struct dya*, void** ptr, int (*compar)(const void *, const void *));
int dya_bs_find(struct dya*, void** ptr, int (*compar)(const void *, const void *), void* key);
void* dya_bs_insert(struct dya*, void** ptr, int (*compar)(const void *, const void *), void* new_element);
void dya_bs_delete(struct dya*, void** ptr, int (*compar)(const void *, const void *), void* key);

static inline uint8_t dya_cap(struct dya* d)
{
	return 1 << d->cap_log2;
}

#define DYA_H
#endif
