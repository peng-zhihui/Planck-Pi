#ifndef _CACHE_H
#define _CACHE_H
int flush_clean_user_range(long start, long end);
int flush_user_range(long start, long end);
void flush_dcache_all(void);
#endif