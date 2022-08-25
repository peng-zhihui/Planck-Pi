// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (C) 2011, Red Hat Inc, Arnaldo Carvalho de Melo <acme@redhat.com>
 *
 * Parts came from builtin-{top,stat,record}.c, see those files for further
 * copyright notes.
 */
#include <api/fs/fs.h>
#include <errno.h>
#include <inttypes.h>
#include <poll.h>
#include "cpumap.h"
#include "util/mmap.h"
#include "thread_map.h"
#include "target.h"
#include "evlist.h"
#include "evsel.h"
#include "debug.h"
#include "units.h"
#include <internal/lib.h> // page_size
#include "../perf.h"
#include "asm/bug.h"
#include "bpf-event.h"
#include <signal.h>
#include <unistd.h>
#include <sched.h>
#include <stdlib.h>

#include "parse-events.h"
#include <subcmd/parse-options.h>

#include <fcntl.h>
#include <sys/ioctl.h>
#include <sys/mman.h>

#include <linux/bitops.h>
#include <linux/hash.h>
#include <linux/log2.h>
#include <linux/err.h>
#include <linux/string.h>
#include <linux/zalloc.h>
#include <perf/evlist.h>
#include <perf/evsel.h>
#include <perf/cpumap.h>

#include <internal/xyarray.h>

#ifdef LACKS_SIGQUEUE_PROTOTYPE
int sigqueue(pid_t pid, int sig, const union sigval value);
#endif

#define FD(e, x, y) (*(int *)xyarray__entry(e->core.fd, x, y))
#define SID(e, x, y) xyarray__entry(e->core.sample_id, x, y)

void evlist__init(struct evlist *evlist, struct perf_cpu_map *cpus,
		  struct perf_thread_map *threads)
{
	perf_evlist__init(&evlist->core);
	perf_evlist__set_maps(&evlist->core, cpus, threads);
	fdarray__init(&evlist->core.pollfd, 64);
	evlist->workload.pid = -1;
	evlist->bkw_mmap_state = BKW_MMAP_NOTREADY;
}

struct evlist *evlist__new(void)
{
	struct evlist *evlist = zalloc(sizeof(*evlist));

	if (evlist != NULL)
		evlist__init(evlist, NULL, NULL);

	return evlist;
}

struct evlist *perf_evlist__new_default(void)
{
	struct evlist *evlist = evlist__new();

	if (evlist && perf_evlist__add_default(evlist)) {
		evlist__delete(evlist);
		evlist = NULL;
	}

	return evlist;
}

struct evlist *perf_evlist__new_dummy(void)
{
	struct evlist *evlist = evlist__new();

	if (evlist && perf_evlist__add_dummy(evlist)) {
		evlist__delete(evlist);
		evlist = NULL;
	}

	return evlist;
}

/**
 * perf_evlist__set_id_pos - set the positions of event ids.
 * @evlist: selected event list
 *
 * Events with compatible sample types all have the same id_pos
 * and is_pos.  For convenience, put a copy on evlist.
 */
void perf_evlist__set_id_pos(struct evlist *evlist)
{
	struct evsel *first = evlist__first(evlist);

	evlist->id_pos = first->id_pos;
	evlist->is_pos = first->is_pos;
}

static void perf_evlist__update_id_pos(struct evlist *evlist)
{
	struct evsel *evsel;

	evlist__for_each_entry(evlist, evsel)
		perf_evsel__calc_id_pos(evsel);

	perf_evlist__set_id_pos(evlist);
}

static void evlist__purge(struct evlist *evlist)
{
	struct evsel *pos, *n;

	evlist__for_each_entry_safe(evlist, n, pos) {
		list_del_init(&pos->core.node);
		pos->evlist = NULL;
		evsel__delete(pos);
	}

	evlist->core.nr_entries = 0;
}

void evlist__exit(struct evlist *evlist)
{
	zfree(&evlist->mmap);
	zfree(&evlist->overwrite_mmap);
	fdarray__exit(&evlist->core.pollfd);
}

void evlist__delete(struct evlist *evlist)
{
	if (evlist == NULL)
		return;

	evlist__munmap(evlist);
	evlist__close(evlist);
	perf_cpu_map__put(evlist->core.cpus);
	perf_thread_map__put(evlist->core.threads);
	evlist->core.cpus = NULL;
	evlist->core.threads = NULL;
	evlist__purge(evlist);
	evlist__exit(evlist);
	free(evlist);
}

void evlist__add(struct evlist *evlist, struct evsel *entry)
{
	entry->evlist = evlist;
	entry->idx = evlist->core.nr_entries;
	entry->tracking = !entry->idx;

	perf_evlist__add(&evlist->core, &entry->core);

	if (evlist->core.nr_entries == 1)
		perf_evlist__set_id_pos(evlist);
}

void evlist__remove(struct evlist *evlist, struct evsel *evsel)
{
	evsel->evlist = NULL;
	perf_evlist__remove(&evlist->core, &evsel->core);
}

void perf_evlist__splice_list_tail(struct evlist *evlist,
				   struct list_head *list)
{
	struct evsel *evsel, *temp;

	__evlist__for_each_entry_safe(list, temp, evsel) {
		list_del_init(&evsel->core.node);
		evlist__add(evlist, evsel);
	}
}

void __perf_evlist__set_leader(struct list_head *list)
{
	struct evsel *evsel, *leader;

	leader = list_entry(list->next, struct evsel, core.node);
	evsel = list_entry(list->prev, struct evsel, core.node);

	leader->core.nr_members = evsel->idx - leader->idx + 1;

	__evlist__for_each_entry(list, evsel) {
		evsel->leader = leader;
	}
}

void perf_evlist__set_leader(struct evlist *evlist)
{
	if (evlist->core.nr_entries) {
		evlist->nr_groups = evlist->core.nr_entries > 1 ? 1 : 0;
		__perf_evlist__set_leader(&evlist->core.entries);
	}
}

int __perf_evlist__add_default(struct evlist *evlist, bool precise)
{
	struct evsel *evsel = perf_evsel__new_cycles(precise);

	if (evsel == NULL)
		return -ENOMEM;

	evlist__add(evlist, evsel);
	return 0;
}

int perf_evlist__add_dummy(struct evlist *evlist)
{
	struct perf_event_attr attr = {
		.type	= PERF_TYPE_SOFTWARE,
		.config = PERF_COUNT_SW_DUMMY,
		.size	= sizeof(attr), /* to capture ABI version */
	};
	struct evsel *evsel = perf_evsel__new_idx(&attr, evlist->core.nr_entries);

	if (evsel == NULL)
		return -ENOMEM;

	evlist__add(evlist, evsel);
	return 0;
}

static int evlist__add_attrs(struct evlist *evlist,
				  struct perf_event_attr *attrs, size_t nr_attrs)
{
	struct evsel *evsel, *n;
	LIST_HEAD(head);
	size_t i;

	for (i = 0; i < nr_attrs; i++) {
		evsel = perf_evsel__new_idx(attrs + i, evlist->core.nr_entries + i);
		if (evsel == NULL)
			goto out_delete_partial_list;
		list_add_tail(&evsel->core.node, &head);
	}

	perf_evlist__splice_list_tail(evlist, &head);

	return 0;

out_delete_partial_list:
	__evlist__for_each_entry_safe(&head, n, evsel)
		evsel__delete(evsel);
	return -1;
}

int __perf_evlist__add_default_attrs(struct evlist *evlist,
				     struct perf_event_attr *attrs, size_t nr_attrs)
{
	size_t i;

	for (i = 0; i < nr_attrs; i++)
		event_attr_init(attrs + i);

	return evlist__add_attrs(evlist, attrs, nr_attrs);
}

struct evsel *
perf_evlist__find_tracepoint_by_id(struct evlist *evlist, int id)
{
	struct evsel *evsel;

	evlist__for_each_entry(evlist, evsel) {
		if (evsel->core.attr.type   == PERF_TYPE_TRACEPOINT &&
		    (int)evsel->core.attr.config == id)
			return evsel;
	}

	return NULL;
}

struct evsel *
perf_evlist__find_tracepoint_by_name(struct evlist *evlist,
				     const char *name)
{
	struct evsel *evsel;

	evlist__for_each_entry(evlist, evsel) {
		if ((evsel->core.attr.type == PERF_TYPE_TRACEPOINT) &&
		    (strcmp(evsel->name, name) == 0))
			return evsel;
	}

	return NULL;
}

int perf_evlist__add_newtp(struct evlist *evlist,
			   const char *sys, const char *name, void *handler)
{
	struct evsel *evsel = perf_evsel__newtp(sys, name);

	if (IS_ERR(evsel))
		return -1;

	evsel->handler = handler;
	evlist__add(evlist, evsel);
	return 0;
}

static int perf_evlist__nr_threads(struct evlist *evlist,
				   struct evsel *evsel)
{
	if (evsel->core.system_wide)
		return 1;
	else
		return perf_thread_map__nr(evlist->core.threads);
}

void evlist__disable(struct evlist *evlist)
{
	struct evsel *pos;

	evlist__for_each_entry(evlist, pos) {
		if (pos->disabled || !perf_evsel__is_group_leader(pos) || !pos->core.fd)
			continue;
		evsel__disable(pos);
	}

	evlist->enabled = false;
}

void evlist__enable(struct evlist *evlist)
{
	struct evsel *pos;

	evlist__for_each_entry(evlist, pos) {
		if (!perf_evsel__is_group_leader(pos) || !pos->core.fd)
			continue;
		evsel__enable(pos);
	}

	evlist->enabled = true;
}

void perf_evlist__toggle_enable(struct evlist *evlist)
{
	(evlist->enabled ? evlist__disable : evlist__enable)(evlist);
}

static int perf_evlist__enable_event_cpu(struct evlist *evlist,
					 struct evsel *evsel, int cpu)
{
	int thread;
	int nr_threads = perf_evlist__nr_threads(evlist, evsel);

	if (!evsel->core.fd)
		return -EINVAL;

	for (thread = 0; thread < nr_threads; thread++) {
		int err = ioctl(FD(evsel, cpu, thread), PERF_EVENT_IOC_ENABLE, 0);
		if (err)
			return err;
	}
	return 0;
}

static int perf_evlist__enable_event_thread(struct evlist *evlist,
					    struct evsel *evsel,
					    int thread)
{
	int cpu;
	int nr_cpus = perf_cpu_map__nr(evlist->core.cpus);

	if (!evsel->core.fd)
		return -EINVAL;

	for (cpu = 0; cpu < nr_cpus; cpu++) {
		int err = ioctl(FD(evsel, cpu, thread), PERF_EVENT_IOC_ENABLE, 0);
		if (err)
			return err;
	}
	return 0;
}

int perf_evlist__enable_event_idx(struct evlist *evlist,
				  struct evsel *evsel, int idx)
{
	bool per_cpu_mmaps = !perf_cpu_map__empty(evlist->core.cpus);

	if (per_cpu_mmaps)
		return perf_evlist__enable_event_cpu(evlist, evsel, idx);
	else
		return perf_evlist__enable_event_thread(evlist, evsel, idx);
}

int evlist__add_pollfd(struct evlist *evlist, int fd)
{
	return perf_evlist__add_pollfd(&evlist->core, fd, NULL, POLLIN);
}

static void perf_evlist__munmap_filtered(struct fdarray *fda, int fd,
					 void *arg __maybe_unused)
{
	struct mmap *map = fda->priv[fd].ptr;

	if (map)
		perf_mmap__put(map);
}

int evlist__filter_pollfd(struct evlist *evlist, short revents_and_mask)
{
	return fdarray__filter(&evlist->core.pollfd, revents_and_mask,
			       perf_evlist__munmap_filtered, NULL);
}

int evlist__poll(struct evlist *evlist, int timeout)
{
	return perf_evlist__poll(&evlist->core, timeout);
}

static void perf_evlist__set_sid_idx(struct evlist *evlist,
				     struct evsel *evsel, int idx, int cpu,
				     int thread)
{
	struct perf_sample_id *sid = SID(evsel, cpu, thread);
	sid->idx = idx;
	if (evlist->core.cpus && cpu >= 0)
		sid->cpu = evlist->core.cpus->map[cpu];
	else
		sid->cpu = -1;
	if (!evsel->core.system_wide && evlist->core.threads && thread >= 0)
		sid->tid = perf_thread_map__pid(evlist->core.threads, thread);
	else
		sid->tid = -1;
}

struct perf_sample_id *perf_evlist__id2sid(struct evlist *evlist, u64 id)
{
	struct hlist_head *head;
	struct perf_sample_id *sid;
	int hash;

	hash = hash_64(id, PERF_EVLIST__HLIST_BITS);
	head = &evlist->core.heads[hash];

	hlist_for_each_entry(sid, head, node)
		if (sid->id == id)
			return sid;

	return NULL;
}

struct evsel *perf_evlist__id2evsel(struct evlist *evlist, u64 id)
{
	struct perf_sample_id *sid;

	if (evlist->core.nr_entries == 1 || !id)
		return evlist__first(evlist);

	sid = perf_evlist__id2sid(evlist, id);
	if (sid)
		return container_of(sid->evsel, struct evsel, core);

	if (!perf_evlist__sample_id_all(evlist))
		return evlist__first(evlist);

	return NULL;
}

struct evsel *perf_evlist__id2evsel_strict(struct evlist *evlist,
						u64 id)
{
	struct perf_sample_id *sid;

	if (!id)
		return NULL;

	sid = perf_evlist__id2sid(evlist, id);
	if (sid)
		return container_of(sid->evsel, struct evsel, core);

	return NULL;
}

static int perf_evlist__event2id(struct evlist *evlist,
				 union perf_event *event, u64 *id)
{
	const __u64 *array = event->sample.array;
	ssize_t n;

	n = (event->header.size - sizeof(event->header)) >> 3;

	if (event->header.type == PERF_RECORD_SAMPLE) {
		if (evlist->id_pos >= n)
			return -1;
		*id = array[evlist->id_pos];
	} else {
		if (evlist->is_pos > n)
			return -1;
		n -= evlist->is_pos;
		*id = array[n];
	}
	return 0;
}

struct evsel *perf_evlist__event2evsel(struct evlist *evlist,
					    union perf_event *event)
{
	struct evsel *first = evlist__first(evlist);
	struct hlist_head *head;
	struct perf_sample_id *sid;
	int hash;
	u64 id;

	if (evlist->core.nr_entries == 1)
		return first;

	if (!first->core.attr.sample_id_all &&
	    event->header.type != PERF_RECORD_SAMPLE)
		return first;

	if (perf_evlist__event2id(evlist, event, &id))
		return NULL;

	/* Synthesized events have an id of zero */
	if (!id)
		return first;

	hash = hash_64(id, PERF_EVLIST__HLIST_BITS);
	head = &evlist->core.heads[hash];

	hlist_for_each_entry(sid, head, node) {
		if (sid->id == id)
			return container_of(sid->evsel, struct evsel, core);
	}
	return NULL;
}

static int perf_evlist__set_paused(struct evlist *evlist, bool value)
{
	int i;

	if (!evlist->overwrite_mmap)
		return 0;

	for (i = 0; i < evlist->core.nr_mmaps; i++) {
		int fd = evlist->overwrite_mmap[i].core.fd;
		int err;

		if (fd < 0)
			continue;
		err = ioctl(fd, PERF_EVENT_IOC_PAUSE_OUTPUT, value ? 1 : 0);
		if (err)
			return err;
	}
	return 0;
}

static int perf_evlist__pause(struct evlist *evlist)
{
	return perf_evlist__set_paused(evlist, true);
}

static int perf_evlist__resume(struct evlist *evlist)
{
	return perf_evlist__set_paused(evlist, false);
}

static void evlist__munmap_nofree(struct evlist *evlist)
{
	int i;

	if (evlist->mmap)
		for (i = 0; i < evlist->core.nr_mmaps; i++)
			perf_mmap__munmap(&evlist->mmap[i]);

	if (evlist->overwrite_mmap)
		for (i = 0; i < evlist->core.nr_mmaps; i++)
			perf_mmap__munmap(&evlist->overwrite_mmap[i]);
}

void evlist__munmap(struct evlist *evlist)
{
	evlist__munmap_nofree(evlist);
	zfree(&evlist->mmap);
	zfree(&evlist->overwrite_mmap);
}

static struct mmap *evlist__alloc_mmap(struct evlist *evlist,
				       bool overwrite)
{
	int i;
	struct mmap *map;

	evlist->core.nr_mmaps = perf_cpu_map__nr(evlist->core.cpus);
	if (perf_cpu_map__empty(evlist->core.cpus))
		evlist->core.nr_mmaps = perf_thread_map__nr(evlist->core.threads);
	map = zalloc(evlist->core.nr_mmaps * sizeof(struct mmap));
	if (!map)
		return NULL;

	for (i = 0; i < evlist->core.nr_mmaps; i++) {
		map[i].core.fd = -1;
		map[i].core.overwrite = overwrite;
		/*
		 * When the perf_mmap() call is made we grab one refcount, plus
		 * one extra to let perf_mmap__consume() get the last
		 * events after all real references (perf_mmap__get()) are
		 * dropped.
		 *
		 * Each PERF_EVENT_IOC_SET_OUTPUT points to this mmap and
		 * thus does perf_mmap__get() on it.
		 */
		refcount_set(&map[i].core.refcnt, 0);
	}
	return map;
}

static bool
perf_evlist__should_poll(struct evlist *evlist __maybe_unused,
			 struct evsel *evsel)
{
	if (evsel->core.attr.write_backward)
		return false;
	return true;
}

static int evlist__mmap_per_evsel(struct evlist *evlist, int idx,
				       struct mmap_params *mp, int cpu_idx,
				       int thread, int *_output, int *_output_overwrite)
{
	struct evsel *evsel;
	int revent;
	int evlist_cpu = cpu_map__cpu(evlist->core.cpus, cpu_idx);

	evlist__for_each_entry(evlist, evsel) {
		struct mmap *maps = evlist->mmap;
		int *output = _output;
		int fd;
		int cpu;

		mp->prot = PROT_READ | PROT_WRITE;
		if (evsel->core.attr.write_backward) {
			output = _output_overwrite;
			maps = evlist->overwrite_mmap;

			if (!maps) {
				maps = evlist__alloc_mmap(evlist, true);
				if (!maps)
					return -1;
				evlist->overwrite_mmap = maps;
				if (evlist->bkw_mmap_state == BKW_MMAP_NOTREADY)
					perf_evlist__toggle_bkw_mmap(evlist, BKW_MMAP_RUNNING);
			}
			mp->prot &= ~PROT_WRITE;
		}

		if (evsel->core.system_wide && thread)
			continue;

		cpu = perf_cpu_map__idx(evsel->core.cpus, evlist_cpu);
		if (cpu == -1)
			continue;

		fd = FD(evsel, cpu, thread);

		if (*output == -1) {
			*output = fd;

			if (perf_mmap__mmap(&maps[idx], mp, *output, evlist_cpu) < 0)
				return -1;
		} else {
			if (ioctl(fd, PERF_EVENT_IOC_SET_OUTPUT, *output) != 0)
				return -1;

			perf_mmap__get(&maps[idx]);
		}

		revent = perf_evlist__should_poll(evlist, evsel) ? POLLIN : 0;

		/*
		 * The system_wide flag causes a selected event to be opened
		 * always without a pid.  Consequently it will never get a
		 * POLLHUP, but it is used for tracking in combination with
		 * other events, so it should not need to be polled anyway.
		 * Therefore don't add it for polling.
		 */
		if (!evsel->core.system_wide &&
		     perf_evlist__add_pollfd(&evlist->core, fd, &maps[idx], revent) < 0) {
			perf_mmap__put(&maps[idx]);
			return -1;
		}

		if (evsel->core.attr.read_format & PERF_FORMAT_ID) {
			if (perf_evlist__id_add_fd(&evlist->core, &evsel->core, cpu, thread,
						   fd) < 0)
				return -1;
			perf_evlist__set_sid_idx(evlist, evsel, idx, cpu,
						 thread);
		}
	}

	return 0;
}

static int evlist__mmap_per_cpu(struct evlist *evlist,
				     struct mmap_params *mp)
{
	int cpu, thread;
	int nr_cpus = perf_cpu_map__nr(evlist->core.cpus);
	int nr_threads = perf_thread_map__nr(evlist->core.threads);

	pr_debug2("perf event ring buffer mmapped per cpu\n");
	for (cpu = 0; cpu < nr_cpus; cpu++) {
		int output = -1;
		int output_overwrite = -1;

		auxtrace_mmap_params__set_idx(&mp->auxtrace_mp, evlist, cpu,
					      true);

		for (thread = 0; thread < nr_threads; thread++) {
			if (evlist__mmap_per_evsel(evlist, cpu, mp, cpu,
							thread, &output, &output_overwrite))
				goto out_unmap;
		}
	}

	return 0;

out_unmap:
	evlist__munmap_nofree(evlist);
	return -1;
}

static int evlist__mmap_per_thread(struct evlist *evlist,
					struct mmap_params *mp)
{
	int thread;
	int nr_threads = perf_thread_map__nr(evlist->core.threads);

	pr_debug2("perf event ring buffer mmapped per thread\n");
	for (thread = 0; thread < nr_threads; thread++) {
		int output = -1;
		int output_overwrite = -1;

		auxtrace_mmap_params__set_idx(&mp->auxtrace_mp, evlist, thread,
					      false);

		if (evlist__mmap_per_evsel(evlist, thread, mp, 0, thread,
						&output, &output_overwrite))
			goto out_unmap;
	}

	return 0;

out_unmap:
	evlist__munmap_nofree(evlist);
	return -1;
}

unsigned long perf_event_mlock_kb_in_pages(void)
{
	unsigned long pages;
	int max;

	if (sysctl__read_int("kernel/perf_event_mlock_kb", &max) < 0) {
		/*
		 * Pick a once upon a time good value, i.e. things look
		 * strange since we can't read a sysctl value, but lets not
		 * die yet...
		 */
		max = 512;
	} else {
		max -= (page_size / 1024);
	}

	pages = (max * 1024) / page_size;
	if (!is_power_of_2(pages))
		pages = rounddown_pow_of_two(pages);

	return pages;
}

size_t evlist__mmap_size(unsigned long pages)
{
	if (pages == UINT_MAX)
		pages = perf_event_mlock_kb_in_pages();
	else if (!is_power_of_2(pages))
		return 0;

	return (pages + 1) * page_size;
}

static long parse_pages_arg(const char *str, unsigned long min,
			    unsigned long max)
{
	unsigned long pages, val;
	static struct parse_tag tags[] = {
		{ .tag  = 'B', .mult = 1       },
		{ .tag  = 'K', .mult = 1 << 10 },
		{ .tag  = 'M', .mult = 1 << 20 },
		{ .tag  = 'G', .mult = 1 << 30 },
		{ .tag  = 0 },
	};

	if (str == NULL)
		return -EINVAL;

	val = parse_tag_value(str, tags);
	if (val != (unsigned long) -1) {
		/* we got file size value */
		pages = PERF_ALIGN(val, page_size) / page_size;
	} else {
		/* we got pages count value */
		char *eptr;
		pages = strtoul(str, &eptr, 10);
		if (*eptr != '\0')
			return -EINVAL;
	}

	if (pages == 0 && min == 0) {
		/* leave number of pages at 0 */
	} else if (!is_power_of_2(pages)) {
		char buf[100];

		/* round pages up to next power of 2 */
		pages = roundup_pow_of_two(pages);
		if (!pages)
			return -EINVAL;

		unit_number__scnprintf(buf, sizeof(buf), pages * page_size);
		pr_info("rounding mmap pages size to %s (%lu pages)\n",
			buf, pages);
	}

	if (pages > max)
		return -EINVAL;

	return pages;
}

int __perf_evlist__parse_mmap_pages(unsigned int *mmap_pages, const char *str)
{
	unsigned long max = UINT_MAX;
	long pages;

	if (max > SIZE_MAX / page_size)
		max = SIZE_MAX / page_size;

	pages = parse_pages_arg(str, 1, max);
	if (pages < 0) {
		pr_err("Invalid argument for --mmap_pages/-m\n");
		return -1;
	}

	*mmap_pages = pages;
	return 0;
}

int perf_evlist__parse_mmap_pages(const struct option *opt, const char *str,
				  int unset __maybe_unused)
{
	return __perf_evlist__parse_mmap_pages(opt->value, str);
}

/**
 * evlist__mmap_ex - Create mmaps to receive events.
 * @evlist: list of events
 * @pages: map length in pages
 * @overwrite: overwrite older events?
 * @auxtrace_pages - auxtrace map length in pages
 * @auxtrace_overwrite - overwrite older auxtrace data?
 *
 * If @overwrite is %false the user needs to signal event consumption using
 * perf_mmap__write_tail().  Using evlist__mmap_read() does this
 * automatically.
 *
 * Similarly, if @auxtrace_overwrite is %false the user needs to signal data
 * consumption using auxtrace_mmap__write_tail().
 *
 * Return: %0 on success, negative error code otherwise.
 */
int evlist__mmap_ex(struct evlist *evlist, unsigned int pages,
			 unsigned int auxtrace_pages,
			 bool auxtrace_overwrite, int nr_cblocks, int affinity, int flush,
			 int comp_level)
{
	struct evsel *evsel;
	const struct perf_cpu_map *cpus = evlist->core.cpus;
	const struct perf_thread_map *threads = evlist->core.threads;
	/*
	 * Delay setting mp.prot: set it before calling perf_mmap__mmap.
	 * Its value is decided by evsel's write_backward.
	 * So &mp should not be passed through const pointer.
	 */
	struct mmap_params mp = { .nr_cblocks = nr_cblocks, .affinity = affinity, .flush = flush,
				  .comp_level = comp_level };

	if (!evlist->mmap)
		evlist->mmap = evlist__alloc_mmap(evlist, false);
	if (!evlist->mmap)
		return -ENOMEM;

	if (evlist->core.pollfd.entries == NULL && perf_evlist__alloc_pollfd(&evlist->core) < 0)
		return -ENOMEM;

	evlist->core.mmap_len = evlist__mmap_size(pages);
	pr_debug("mmap size %zuB\n", evlist->core.mmap_len);
	mp.mask = evlist->core.mmap_len - page_size - 1;

	auxtrace_mmap_params__init(&mp.auxtrace_mp, evlist->core.mmap_len,
				   auxtrace_pages, auxtrace_overwrite);

	evlist__for_each_entry(evlist, evsel) {
		if ((evsel->core.attr.read_format & PERF_FORMAT_ID) &&
		    evsel->core.sample_id == NULL &&
		    perf_evsel__alloc_id(&evsel->core, perf_cpu_map__nr(cpus), threads->nr) < 0)
			return -ENOMEM;
	}

	if (perf_cpu_map__empty(cpus))
		return evlist__mmap_per_thread(evlist, &mp);

	return evlist__mmap_per_cpu(evlist, &mp);
}

int evlist__mmap(struct evlist *evlist, unsigned int pages)
{
	return evlist__mmap_ex(evlist, pages, 0, false, 0, PERF_AFFINITY_SYS, 1, 0);
}

int perf_evlist__create_maps(struct evlist *evlist, struct target *target)
{
	bool all_threads = (target->per_thread && target->system_wide);
	struct perf_cpu_map *cpus;
	struct perf_thread_map *threads;

	/*
	 * If specify '-a' and '--per-thread' to perf record, perf record
	 * will override '--per-thread'. target->per_thread = false and
	 * target->system_wide = true.
	 *
	 * If specify '--per-thread' only to perf record,
	 * target->per_thread = true and target->system_wide = false.
	 *
	 * So target->per_thread && target->system_wide is false.
	 * For perf record, thread_map__new_str doesn't call
	 * thread_map__new_all_cpus. That will keep perf record's
	 * current behavior.
	 *
	 * For perf stat, it allows the case that target->per_thread and
	 * target->system_wide are all true. It means to collect system-wide
	 * per-thread data. thread_map__new_str will call
	 * thread_map__new_all_cpus to enumerate all threads.
	 */
	threads = thread_map__new_str(target->pid, target->tid, target->uid,
				      all_threads);

	if (!threads)
		return -1;

	if (target__uses_dummy_map(target))
		cpus = perf_cpu_map__dummy_new();
	else
		cpus = perf_cpu_map__new(target->cpu_list);

	if (!cpus)
		goto out_delete_threads;

	evlist->core.has_user_cpus = !!target->cpu_list;

	perf_evlist__set_maps(&evlist->core, cpus, threads);

	/* as evlist now has references, put count here */
	perf_cpu_map__put(cpus);
	perf_thread_map__put(threads);

	return 0;

out_delete_threads:
	perf_thread_map__put(threads);
	return -1;
}

void __perf_evlist__set_sample_bit(struct evlist *evlist,
				   enum perf_event_sample_format bit)
{
	struct evsel *evsel;

	evlist__for_each_entry(evlist, evsel)
		__perf_evsel__set_sample_bit(evsel, bit);
}

void __perf_evlist__reset_sample_bit(struct evlist *evlist,
				     enum perf_event_sample_format bit)
{
	struct evsel *evsel;

	evlist__for_each_entry(evlist, evsel)
		__perf_evsel__reset_sample_bit(evsel, bit);
}

int perf_evlist__apply_filters(struct evlist *evlist, struct evsel **err_evsel)
{
	struct evsel *evsel;
	int err = 0;

	evlist__for_each_entry(evlist, evsel) {
		if (evsel->filter == NULL)
			continue;

		/*
		 * filters only work for tracepoint event, which doesn't have cpu limit.
		 * So evlist and evsel should always be same.
		 */
		err = perf_evsel__apply_filter(&evsel->core, evsel->filter);
		if (err) {
			*err_evsel = evsel;
			break;
		}
	}

	return err;
}

int perf_evlist__set_tp_filter(struct evlist *evlist, const char *filter)
{
	struct evsel *evsel;
	int err = 0;

	evlist__for_each_entry(evlist, evsel) {
		if (evsel->core.attr.type != PERF_TYPE_TRACEPOINT)
			continue;

		err = perf_evsel__set_filter(evsel, filter);
		if (err)
			break;
	}

	return err;
}

int perf_evlist__set_tp_filter_pids(struct evlist *evlist, size_t npids, pid_t *pids)
{
	char *filter;
	int ret = -1;
	size_t i;

	for (i = 0; i < npids; ++i) {
		if (i == 0) {
			if (asprintf(&filter, "common_pid != %d", pids[i]) < 0)
				return -1;
		} else {
			char *tmp;

			if (asprintf(&tmp, "%s && common_pid != %d", filter, pids[i]) < 0)
				goto out_free;

			free(filter);
			filter = tmp;
		}
	}

	ret = perf_evlist__set_tp_filter(evlist, filter);
out_free:
	free(filter);
	return ret;
}

int perf_evlist__set_tp_filter_pid(struct evlist *evlist, pid_t pid)
{
	return perf_evlist__set_tp_filter_pids(evlist, 1, &pid);
}

bool perf_evlist__valid_sample_type(struct evlist *evlist)
{
	struct evsel *pos;

	if (evlist->core.nr_entries == 1)
		return true;

	if (evlist->id_pos < 0 || evlist->is_pos < 0)
		return false;

	evlist__for_each_entry(evlist, pos) {
		if (pos->id_pos != evlist->id_pos ||
		    pos->is_pos != evlist->is_pos)
			return false;
	}

	return true;
}

u64 __perf_evlist__combined_sample_type(struct evlist *evlist)
{
	struct evsel *evsel;

	if (evlist->combined_sample_type)
		return evlist->combined_sample_type;

	evlist__for_each_entry(evlist, evsel)
		evlist->combined_sample_type |= evsel->core.attr.sample_type;

	return evlist->combined_sample_type;
}

u64 perf_evlist__combined_sample_type(struct evlist *evlist)
{
	evlist->combined_sample_type = 0;
	return __perf_evlist__combined_sample_type(evlist);
}

u64 perf_evlist__combined_branch_type(struct evlist *evlist)
{
	struct evsel *evsel;
	u64 branch_type = 0;

	evlist__for_each_entry(evlist, evsel)
		branch_type |= evsel->core.attr.branch_sample_type;
	return branch_type;
}

bool perf_evlist__valid_read_format(struct evlist *evlist)
{
	struct evsel *first = evlist__first(evlist), *pos = first;
	u64 read_format = first->core.attr.read_format;
	u64 sample_type = first->core.attr.sample_type;

	evlist__for_each_entry(evlist, pos) {
		if (read_format != pos->core.attr.read_format)
			return false;
	}

	/* PERF_SAMPLE_READ imples PERF_FORMAT_ID. */
	if ((sample_type & PERF_SAMPLE_READ) &&
	    !(read_format & PERF_FORMAT_ID)) {
		return false;
	}

	return true;
}

u16 perf_evlist__id_hdr_size(struct evlist *evlist)
{
	struct evsel *first = evlist__first(evlist);
	struct perf_sample *data;
	u64 sample_type;
	u16 size = 0;

	if (!first->core.attr.sample_id_all)
		goto out;

	sample_type = first->core.attr.sample_type;

	if (sample_type & PERF_SAMPLE_TID)
		size += sizeof(data->tid) * 2;

       if (sample_type & PERF_SAMPLE_TIME)
		size += sizeof(data->time);

	if (sample_type & PERF_SAMPLE_ID)
		size += sizeof(data->id);

	if (sample_type & PERF_SAMPLE_STREAM_ID)
		size += sizeof(data->stream_id);

	if (sample_type & PERF_SAMPLE_CPU)
		size += sizeof(data->cpu) * 2;

	if (sample_type & PERF_SAMPLE_IDENTIFIER)
		size += sizeof(data->id);
out:
	return size;
}

bool perf_evlist__valid_sample_id_all(struct evlist *evlist)
{
	struct evsel *first = evlist__first(evlist), *pos = first;

	evlist__for_each_entry_continue(evlist, pos) {
		if (first->core.attr.sample_id_all != pos->core.attr.sample_id_all)
			return false;
	}

	return true;
}

bool perf_evlist__sample_id_all(struct evlist *evlist)
{
	struct evsel *first = evlist__first(evlist);
	return first->core.attr.sample_id_all;
}

void perf_evlist__set_selected(struct evlist *evlist,
			       struct evsel *evsel)
{
	evlist->selected = evsel;
}

void evlist__close(struct evlist *evlist)
{
	struct evsel *evsel;

	evlist__for_each_entry_reverse(evlist, evsel)
		evsel__close(evsel);
}

static int perf_evlist__create_syswide_maps(struct evlist *evlist)
{
	struct perf_cpu_map *cpus;
	struct perf_thread_map *threads;
	int err = -ENOMEM;

	/*
	 * Try reading /sys/devices/system/cpu/online to get
	 * an all cpus map.
	 *
	 * FIXME: -ENOMEM is the best we can do here, the cpu_map
	 * code needs an overhaul to properly forward the
	 * error, and we may not want to do that fallback to a
	 * default cpu identity map :-\
	 */
	cpus = perf_cpu_map__new(NULL);
	if (!cpus)
		goto out;

	threads = perf_thread_map__new_dummy();
	if (!threads)
		goto out_put;

	perf_evlist__set_maps(&evlist->core, cpus, threads);

	perf_thread_map__put(threads);
out_put:
	perf_cpu_map__put(cpus);
out:
	return err;
}

int evlist__open(struct evlist *evlist)
{
	struct evsel *evsel;
	int err;

	/*
	 * Default: one fd per CPU, all threads, aka systemwide
	 * as sys_perf_event_open(cpu = -1, thread = -1) is EINVAL
	 */
	if (evlist->core.threads == NULL && evlist->core.cpus == NULL) {
		err = perf_evlist__create_syswide_maps(evlist);
		if (err < 0)
			goto out_err;
	}

	perf_evlist__update_id_pos(evlist);

	evlist__for_each_entry(evlist, evsel) {
		err = evsel__open(evsel, evsel->core.cpus, evsel->core.threads);
		if (err < 0)
			goto out_err;
	}

	return 0;
out_err:
	evlist__close(evlist);
	errno = -err;
	return err;
}

int perf_evlist__prepare_workload(struct evlist *evlist, struct target *target,
				  const char *argv[], bool pipe_output,
				  void (*exec_error)(int signo, siginfo_t *info, void *ucontext))
{
	int child_ready_pipe[2], go_pipe[2];
	char bf;

	if (pipe(child_ready_pipe) < 0) {
		perror("failed to create 'ready' pipe");
		return -1;
	}

	if (pipe(go_pipe) < 0) {
		perror("failed to create 'go' pipe");
		goto out_close_ready_pipe;
	}

	evlist->workload.pid = fork();
	if (evlist->workload.pid < 0) {
		perror("failed to fork");
		goto out_close_pipes;
	}

	if (!evlist->workload.pid) {
		int ret;

		if (pipe_output)
			dup2(2, 1);

		signal(SIGTERM, SIG_DFL);

		close(child_ready_pipe[0]);
		close(go_pipe[1]);
		fcntl(go_pipe[0], F_SETFD, FD_CLOEXEC);

		/*
		 * Tell the parent we're ready to go
		 */
		close(child_ready_pipe[1]);

		/*
		 * Wait until the parent tells us to go.
		 */
		ret = read(go_pipe[0], &bf, 1);
		/*
		 * The parent will ask for the execvp() to be performed by
		 * writing exactly one byte, in workload.cork_fd, usually via
		 * perf_evlist__start_workload().
		 *
		 * For cancelling the workload without actually running it,
		 * the parent will just close workload.cork_fd, without writing
		 * anything, i.e. read will return zero and we just exit()
		 * here.
		 */
		if (ret != 1) {
			if (ret == -1)
				perror("unable to read pipe");
			exit(ret);
		}

		execvp(argv[0], (char **)argv);

		if (exec_error) {
			union sigval val;

			val.sival_int = errno;
			if (sigqueue(getppid(), SIGUSR1, val))
				perror(argv[0]);
		} else
			perror(argv[0]);
		exit(-1);
	}

	if (exec_error) {
		struct sigaction act = {
			.sa_flags     = SA_SIGINFO,
			.sa_sigaction = exec_error,
		};
		sigaction(SIGUSR1, &act, NULL);
	}

	if (target__none(target)) {
		if (evlist->core.threads == NULL) {
			fprintf(stderr, "FATAL: evlist->threads need to be set at this point (%s:%d).\n",
				__func__, __LINE__);
			goto out_close_pipes;
		}
		perf_thread_map__set_pid(evlist->core.threads, 0, evlist->workload.pid);
	}

	close(child_ready_pipe[1]);
	close(go_pipe[0]);
	/*
	 * wait for child to settle
	 */
	if (read(child_ready_pipe[0], &bf, 1) == -1) {
		perror("unable to read pipe");
		goto out_close_pipes;
	}

	fcntl(go_pipe[1], F_SETFD, FD_CLOEXEC);
	evlist->workload.cork_fd = go_pipe[1];
	close(child_ready_pipe[0]);
	return 0;

out_close_pipes:
	close(go_pipe[0]);
	close(go_pipe[1]);
out_close_ready_pipe:
	close(child_ready_pipe[0]);
	close(child_ready_pipe[1]);
	return -1;
}

int perf_evlist__start_workload(struct evlist *evlist)
{
	if (evlist->workload.cork_fd > 0) {
		char bf = 0;
		int ret;
		/*
		 * Remove the cork, let it rip!
		 */
		ret = write(evlist->workload.cork_fd, &bf, 1);
		if (ret < 0)
			perror("unable to write to pipe");

		close(evlist->workload.cork_fd);
		return ret;
	}

	return 0;
}

int perf_evlist__parse_sample(struct evlist *evlist, union perf_event *event,
			      struct perf_sample *sample)
{
	struct evsel *evsel = perf_evlist__event2evsel(evlist, event);

	if (!evsel)
		return -EFAULT;
	return perf_evsel__parse_sample(evsel, event, sample);
}

int perf_evlist__parse_sample_timestamp(struct evlist *evlist,
					union perf_event *event,
					u64 *timestamp)
{
	struct evsel *evsel = perf_evlist__event2evsel(evlist, event);

	if (!evsel)
		return -EFAULT;
	return perf_evsel__parse_sample_timestamp(evsel, event, timestamp);
}

int perf_evlist__strerror_open(struct evlist *evlist,
			       int err, char *buf, size_t size)
{
	int printed, value;
	char sbuf[STRERR_BUFSIZE], *emsg = str_error_r(err, sbuf, sizeof(sbuf));

	switch (err) {
	case EACCES:
	case EPERM:
		printed = scnprintf(buf, size,
				    "Error:\t%s.\n"
				    "Hint:\tCheck /proc/sys/kernel/perf_event_paranoid setting.", emsg);

		value = perf_event_paranoid();

		printed += scnprintf(buf + printed, size - printed, "\nHint:\t");

		if (value >= 2) {
			printed += scnprintf(buf + printed, size - printed,
					     "For your workloads it needs to be <= 1\nHint:\t");
		}
		printed += scnprintf(buf + printed, size - printed,
				     "For system wide tracing it needs to be set to -1.\n");

		printed += scnprintf(buf + printed, size - printed,
				    "Hint:\tTry: 'sudo sh -c \"echo -1 > /proc/sys/kernel/perf_event_paranoid\"'\n"
				    "Hint:\tThe current value is %d.", value);
		break;
	case EINVAL: {
		struct evsel *first = evlist__first(evlist);
		int max_freq;

		if (sysctl__read_int("kernel/perf_event_max_sample_rate", &max_freq) < 0)
			goto out_default;

		if (first->core.attr.sample_freq < (u64)max_freq)
			goto out_default;

		printed = scnprintf(buf, size,
				    "Error:\t%s.\n"
				    "Hint:\tCheck /proc/sys/kernel/perf_event_max_sample_rate.\n"
				    "Hint:\tThe current value is %d and %" PRIu64 " is being requested.",
				    emsg, max_freq, first->core.attr.sample_freq);
		break;
	}
	default:
out_default:
		scnprintf(buf, size, "%s", emsg);
		break;
	}

	return 0;
}

int perf_evlist__strerror_mmap(struct evlist *evlist, int err, char *buf, size_t size)
{
	char sbuf[STRERR_BUFSIZE], *emsg = str_error_r(err, sbuf, sizeof(sbuf));
	int pages_attempted = evlist->core.mmap_len / 1024, pages_max_per_user, printed = 0;

	switch (err) {
	case EPERM:
		sysctl__read_int("kernel/perf_event_mlock_kb", &pages_max_per_user);
		printed += scnprintf(buf + printed, size - printed,
				     "Error:\t%s.\n"
				     "Hint:\tCheck /proc/sys/kernel/perf_event_mlock_kb (%d kB) setting.\n"
				     "Hint:\tTried using %zd kB.\n",
				     emsg, pages_max_per_user, pages_attempted);

		if (pages_attempted >= pages_max_per_user) {
			printed += scnprintf(buf + printed, size - printed,
					     "Hint:\tTry 'sudo sh -c \"echo %d > /proc/sys/kernel/perf_event_mlock_kb\"', or\n",
					     pages_max_per_user + pages_attempted);
		}

		printed += scnprintf(buf + printed, size - printed,
				     "Hint:\tTry using a smaller -m/--mmap-pages value.");
		break;
	default:
		scnprintf(buf, size, "%s", emsg);
		break;
	}

	return 0;
}

void perf_evlist__to_front(struct evlist *evlist,
			   struct evsel *move_evsel)
{
	struct evsel *evsel, *n;
	LIST_HEAD(move);

	if (move_evsel == evlist__first(evlist))
		return;

	evlist__for_each_entry_safe(evlist, n, evsel) {
		if (evsel->leader == move_evsel->leader)
			list_move_tail(&evsel->core.node, &move);
	}

	list_splice(&move, &evlist->core.entries);
}

void perf_evlist__set_tracking_event(struct evlist *evlist,
				     struct evsel *tracking_evsel)
{
	struct evsel *evsel;

	if (tracking_evsel->tracking)
		return;

	evlist__for_each_entry(evlist, evsel) {
		if (evsel != tracking_evsel)
			evsel->tracking = false;
	}

	tracking_evsel->tracking = true;
}

struct evsel *
perf_evlist__find_evsel_by_str(struct evlist *evlist,
			       const char *str)
{
	struct evsel *evsel;

	evlist__for_each_entry(evlist, evsel) {
		if (!evsel->name)
			continue;
		if (strcmp(str, evsel->name) == 0)
			return evsel;
	}

	return NULL;
}

void perf_evlist__toggle_bkw_mmap(struct evlist *evlist,
				  enum bkw_mmap_state state)
{
	enum bkw_mmap_state old_state = evlist->bkw_mmap_state;
	enum action {
		NONE,
		PAUSE,
		RESUME,
	} action = NONE;

	if (!evlist->overwrite_mmap)
		return;

	switch (old_state) {
	case BKW_MMAP_NOTREADY: {
		if (state != BKW_MMAP_RUNNING)
			goto state_err;
		break;
	}
	case BKW_MMAP_RUNNING: {
		if (state != BKW_MMAP_DATA_PENDING)
			goto state_err;
		action = PAUSE;
		break;
	}
	case BKW_MMAP_DATA_PENDING: {
		if (state != BKW_MMAP_EMPTY)
			goto state_err;
		break;
	}
	case BKW_MMAP_EMPTY: {
		if (state != BKW_MMAP_RUNNING)
			goto state_err;
		action = RESUME;
		break;
	}
	default:
		WARN_ONCE(1, "Shouldn't get there\n");
	}

	evlist->bkw_mmap_state = state;

	switch (action) {
	case PAUSE:
		perf_evlist__pause(evlist);
		break;
	case RESUME:
		perf_evlist__resume(evlist);
		break;
	case NONE:
	default:
		break;
	}

state_err:
	return;
}

bool perf_evlist__exclude_kernel(struct evlist *evlist)
{
	struct evsel *evsel;

	evlist__for_each_entry(evlist, evsel) {
		if (!evsel->core.attr.exclude_kernel)
			return false;
	}

	return true;
}

/*
 * Events in data file are not collect in groups, but we still want
 * the group display. Set the artificial group and set the leader's
 * forced_leader flag to notify the display code.
 */
void perf_evlist__force_leader(struct evlist *evlist)
{
	if (!evlist->nr_groups) {
		struct evsel *leader = evlist__first(evlist);

		perf_evlist__set_leader(evlist);
		leader->forced_leader = true;
	}
}

struct evsel *perf_evlist__reset_weak_group(struct evlist *evsel_list,
						 struct evsel *evsel)
{
	struct evsel *c2, *leader;
	bool is_open = true;

	leader = evsel->leader;
	pr_debug("Weak group for %s/%d failed\n",
			leader->name, leader->core.nr_members);

	/*
	 * for_each_group_member doesn't work here because it doesn't
	 * include the first entry.
	 */
	evlist__for_each_entry(evsel_list, c2) {
		if (c2 == evsel)
			is_open = false;
		if (c2->leader == leader) {
			if (is_open)
				perf_evsel__close(&c2->core);
			c2->leader = c2;
			c2->core.nr_members = 0;
		}
	}
	return leader;
}

int perf_evlist__add_sb_event(struct evlist **evlist,
			      struct perf_event_attr *attr,
			      perf_evsel__sb_cb_t cb,
			      void *data)
{
	struct evsel *evsel;
	bool new_evlist = (*evlist) == NULL;

	if (*evlist == NULL)
		*evlist = evlist__new();
	if (*evlist == NULL)
		return -1;

	if (!attr->sample_id_all) {
		pr_warning("enabling sample_id_all for all side band events\n");
		attr->sample_id_all = 1;
	}

	evsel = perf_evsel__new_idx(attr, (*evlist)->core.nr_entries);
	if (!evsel)
		goto out_err;

	evsel->side_band.cb = cb;
	evsel->side_band.data = data;
	evlist__add(*evlist, evsel);
	return 0;

out_err:
	if (new_evlist) {
		evlist__delete(*evlist);
		*evlist = NULL;
	}
	return -1;
}

static void *perf_evlist__poll_thread(void *arg)
{
	struct evlist *evlist = arg;
	bool draining = false;
	int i, done = 0;
	/*
	 * In order to read symbols from other namespaces perf to needs to call
	 * setns(2).  This isn't permitted if the struct_fs has multiple users.
	 * unshare(2) the fs so that we may continue to setns into namespaces
	 * that we're observing when, for instance, reading the build-ids at
	 * the end of a 'perf record' session.
	 */
	unshare(CLONE_FS);

	while (!done) {
		bool got_data = false;

		if (evlist->thread.done)
			draining = true;

		if (!draining)
			evlist__poll(evlist, 1000);

		for (i = 0; i < evlist->core.nr_mmaps; i++) {
			struct mmap *map = &evlist->mmap[i];
			union perf_event *event;

			if (perf_mmap__read_init(map))
				continue;
			while ((event = perf_mmap__read_event(map)) != NULL) {
				struct evsel *evsel = perf_evlist__event2evsel(evlist, event);

				if (evsel && evsel->side_band.cb)
					evsel->side_band.cb(event, evsel->side_band.data);
				else
					pr_warning("cannot locate proper evsel for the side band event\n");

				perf_mmap__consume(map);
				got_data = true;
			}
			perf_mmap__read_done(map);
		}

		if (draining && !got_data)
			break;
	}
	return NULL;
}

int perf_evlist__start_sb_thread(struct evlist *evlist,
				 struct target *target)
{
	struct evsel *counter;

	if (!evlist)
		return 0;

	if (perf_evlist__create_maps(evlist, target))
		goto out_delete_evlist;

	evlist__for_each_entry(evlist, counter) {
		if (evsel__open(counter, evlist->core.cpus,
				     evlist->core.threads) < 0)
			goto out_delete_evlist;
	}

	if (evlist__mmap(evlist, UINT_MAX))
		goto out_delete_evlist;

	evlist__for_each_entry(evlist, counter) {
		if (evsel__enable(counter))
			goto out_delete_evlist;
	}

	evlist->thread.done = 0;
	if (pthread_create(&evlist->thread.th, NULL, perf_evlist__poll_thread, evlist))
		goto out_delete_evlist;

	return 0;

out_delete_evlist:
	evlist__delete(evlist);
	evlist = NULL;
	return -1;
}

void perf_evlist__stop_sb_thread(struct evlist *evlist)
{
	if (!evlist)
		return;
	evlist->thread.done = 1;
	pthread_join(evlist->thread.th, NULL);
	evlist__delete(evlist);
}
