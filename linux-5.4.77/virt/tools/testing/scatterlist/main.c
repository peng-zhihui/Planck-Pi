#include <stdio.h>
#include <assert.h>

#include <linux/scatterlist.h>

#define MAX_PAGES (64)

static void set_pages(struct page **pages, const unsigned *array, unsigned num)
{
	unsigned int i;

	assert(num < MAX_PAGES);
	for (i = 0; i < num; i++)
		pages[i] = (struct page *)(unsigned long)
			   ((1 + array[i]) * PAGE_SIZE);
}

#define pfn(...) (unsigned []){ __VA_ARGS__ }

int main(void)
{
	const unsigned int sgmax = SCATTERLIST_MAX_SEGMENT;
	struct test {
		int alloc_ret;
		unsigned num_pages;
		unsigned *pfn;
		unsigned size;
		unsigned int max_seg;
		unsigned int expected_segments;
	} *test, tests[] = {
		{ -EINVAL, 1, pfn(0), PAGE_SIZE, PAGE_SIZE + 1, 1 },
		{ -EINVAL, 1, pfn(0), PAGE_SIZE, 0, 1 },
		{ -EINVAL, 1, pfn(0), PAGE_SIZE, sgmax + 1, 1 },
		{ 0, 1, pfn(0), PAGE_SIZE, sgmax, 1 },
		{ 0, 1, pfn(0), 1, sgmax, 1 },
		{ 0, 2, pfn(0, 1), 2 * PAGE_SIZE, sgmax, 1 },
		{ 0, 2, pfn(1, 0), 2 * PAGE_SIZE, sgmax, 2 },
		{ 0, 3, pfn(0, 1, 2), 3 * PAGE_SIZE, sgmax, 1 },
		{ 0, 3, pfn(0, 2, 1), 3 * PAGE_SIZE, sgmax, 3 },
		{ 0, 3, pfn(0, 1, 3), 3 * PAGE_SIZE, sgmax, 2 },
		{ 0, 3, pfn(1, 2, 4), 3 * PAGE_SIZE, sgmax, 2 },
		{ 0, 3, pfn(1, 3, 4), 3 * PAGE_SIZE, sgmax, 2 },
		{ 0, 4, pfn(0, 1, 3, 4), 4 * PAGE_SIZE, sgmax, 2 },
		{ 0, 5, pfn(0, 1, 3, 4, 5), 5 * PAGE_SIZE, sgmax, 2 },
		{ 0, 5, pfn(0, 1, 3, 4, 6), 5 * PAGE_SIZE, sgmax, 3 },
		{ 0, 5, pfn(0, 1, 2, 3, 4), 5 * PAGE_SIZE, sgmax, 1 },
		{ 0, 5, pfn(0, 1, 2, 3, 4), 5 * PAGE_SIZE, 2 * PAGE_SIZE, 3 },
		{ 0, 6, pfn(0, 1, 2, 3, 4, 5), 6 * PAGE_SIZE, 2 * PAGE_SIZE, 3 },
		{ 0, 6, pfn(0, 2, 3, 4, 5, 6), 6 * PAGE_SIZE, 2 * PAGE_SIZE, 4 },
		{ 0, 6, pfn(0, 1, 3, 4, 5, 6), 6 * PAGE_SIZE, 2 * PAGE_SIZE, 3 },
		{ 0, 0, NULL, 0, 0, 0 },
	};
	unsigned int i;

	for (i = 0, test = tests; test->expected_segments; test++, i++) {
		struct page *pages[MAX_PAGES];
		struct sg_table st;
		int ret;

		set_pages(pages, test->pfn, test->num_pages);

		ret = __sg_alloc_table_from_pages(&st, pages, test->num_pages,
						  0, test->size, test->max_seg,
						  GFP_KERNEL);
		assert(ret == test->alloc_ret);

		if (test->alloc_ret)
			continue;

		assert(st.nents == test->expected_segments);
		assert(st.orig_nents == test->expected_segments);

		sg_free_table(&st);
	}

	assert(i == (sizeof(tests) / sizeof(tests[0])) - 1);

	return 0;
}
