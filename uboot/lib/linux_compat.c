
#include <common.h>
#include <malloc.h>
#include <memalign.h>
#include <asm/cache.h>
#include <linux/compat.h>

struct p_current cur = {
	.pid = 1,
};
__maybe_unused struct p_current *current = &cur;

unsigned long copy_from_user(void *dest, const void *src,
		     unsigned long count)
{
	memcpy((void *)dest, (void *)src, count);
	return 0;
}

void *kmalloc(size_t size, int flags)
{
	void *p;

	p = malloc_cache_aligned(size);
	if (p && flags & __GFP_ZERO)
		memset(p, 0, size);

	return p;
}

struct kmem_cache *get_mem(int element_sz)
{
	struct kmem_cache *ret;

	ret = memalign(ARCH_DMA_MINALIGN, sizeof(struct kmem_cache));
	ret->sz = element_sz;

	return ret;
}

void *kmem_cache_alloc(struct kmem_cache *obj, int flag)
{
	return malloc_cache_aligned(obj->sz);
}

/**
 * kmemdup - duplicate region of memory
 *
 * @src: memory region to duplicate
 * @len: memory region length
 * @gfp: GFP mask to use
 *
 * Return: newly allocated copy of @src or %NULL in case of error
 */
void *kmemdup(const void *src, size_t len, gfp_t gfp)
{
	void *p;

	p = kmalloc(len, gfp);
	if (p)
		memcpy(p, src, len);
	return p;
}
