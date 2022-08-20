/* SPDX-License-Identifier: GPL-2.0 */
#ifndef __PERF_EVLIST_H
#define __PERF_EVLIST_H 1

#include <linux/compiler.h>
#include <linux/kernel.h>
#include <linux/refcount.h>
#include <linux/list.h>
#include <api/fd/array.h>
#include <internal/evlist.h>
#include <internal/evsel.h>
#include "events_stats.h"
#include "evsel.h"
#include <pthread.h>
#include <signal.h>
#include <unistd.h>

struct pollfd;
struct thread_map;
struct perf_cpu_map;
struct record_opts;

/*
 * State machine of bkw_mmap_state:
 *
 *                     .________________(forbid)_____________.
 *                     |                                     V
 * NOTREADY --(0)--> RUNNING --(1)--> DATA_PENDING --(2)--> EMPTY
 *                     ^  ^              |   ^               |
 *                     |  |__(forbid)____/   |___(forbid)___/|
 *                     |                                     |
 *                      \_________________(3)_______________/
 *
 * NOTREADY     : Backward ring buffers are not ready
 * RUNNING      : Backward ring buffers are recording
 * DATA_PENDING : We are required to collect data from backward ring buffers
 * EMPTY        : We have collected data from backward ring buffers.
 *
 * (0): Setup backward ring buffer
 * (1): Pause ring buffers for reading
 * (2): Read from ring buffers
 * (3): Resume ring buffers for recording
 */
enum bkw_mmap_state {
	BKW_MMAP_NOTREADY,
	BKW_MMAP_RUNNING,
	BKW_MMAP_DATA_PENDING,
	BKW_MMAP_EMPTY,
};

struct evlist {
	struct perf_evlist core;
	int		 nr_groups;
	bool		 enabled;
	int		 id_pos;
	int		 is_pos;
	u64		 combined_sample_type;
	enum bkw_mmap_state bkw_mmap_state;
	struct {
		int	cork_fd;
		pid_t	pid;
	} workload;
	struct mmap *mmap;
	struct mmap *overwrite_mmap;
	struct evsel *selected;
	struct events_stats stats;
	struct perf_env	*env;
	void (*trace_event_sample_raw)(struct evlist *evlist,
				       union perf_event *event,
				       struct perf_sample *sample);
	u64		first_sample_time;
	u64		last_sample_time;
	struct {
		pthread_t		th;
		volatile int		done;
	} thread;
};

struct evsel_str_handler {
	const char *name;
	void	   *handler;
};

struct evlist *evlist__new(void);
struct evlist *perf_evlist__new_default(void);
struct evlist *perf_evlist__new_dummy(void);
void evlist__init(struct evlist *evlist, struct perf_cpu_map *cpus,
		  struct perf_thread_map *threads);
void evlist__exit(struct evlist *evlist);
void evlist__delete(struct evlist *evlist);

void evlist__add(struct evlist *evlist, struct evsel *entry);
void evlist__remove(struct evlist *evlist, struct evsel *evsel);

int __perf_evlist__add_default(struct evlist *evlist, bool precise);

static inline int perf_evlist__add_default(struct evlist *evlist)
{
	return __perf_evlist__add_default(evlist, true);
}

int __perf_evlist__add_default_attrs(struct evlist *evlist,
				     struct perf_event_attr *attrs, size_t nr_attrs);

#define perf_evlist__add_default_attrs(evlist, array) \
	__perf_evlist__add_default_attrs(evlist, array, ARRAY_SIZE(array))

int perf_evlist__add_dummy(struct evlist *evlist);

int perf_evlist__add_sb_event(struct evlist **evlist,
			      struct perf_event_attr *attr,
			      perf_evsel__sb_cb_t cb,
			      void *data);
int perf_evlist__start_sb_thread(struct evlist *evlist,
				 struct target *target);
void perf_evlist__stop_sb_thread(struct evlist *evlist);

int perf_evlist__add_newtp(struct evlist *evlist,
			   const char *sys, const char *name, void *handler);

void __perf_evlist__set_sample_bit(struct evlist *evlist,
				   enum perf_event_sample_format bit);
void __perf_evlist__reset_sample_bit(struct evlist *evlist,
				     enum perf_event_sample_format bit);

#define perf_evlist__set_sample_bit(evlist, bit) \
	__perf_evlist__set_sample_bit(evlist, PERF_SAMPLE_##bit)

#define perf_evlist__reset_sample_bit(evlist, bit) \
	__perf_evlist__reset_sample_bit(evlist, PERF_SAMPLE_##bit)

int perf_evlist__set_tp_filter(struct evlist *evlist, const char *filter);
int perf_evlist__set_tp_filter_pid(struct evlist *evlist, pid_t pid);
int perf_evlist__set_tp_filter_pids(struct evlist *evlist, size_t npids, pid_t *pids);

struct evsel *
perf_evlist__find_tracepoint_by_id(struct evlist *evlist, int id);

struct evsel *
perf_evlist__find_tracepoint_by_name(struct evlist *evlist,
				     const char *name);

int evlist__add_pollfd(struct evlist *evlist, int fd);
int evlist__filter_pollfd(struct evlist *evlist, short revents_and_mask);

int evlist__poll(struct evlist *evlist, int timeout);

struct evsel *perf_evlist__id2evsel(struct evlist *evlist, u64 id);
struct evsel *perf_evlist__id2evsel_strict(struct evlist *evlist,
						u64 id);

struct perf_sample_id *perf_evlist__id2sid(struct evlist *evlist, u64 id);

void perf_evlist__toggle_bkw_mmap(struct evlist *evlist, enum bkw_mmap_state state);

void evlist__mmap_consume(struct evlist *evlist, int idx);

int evlist__open(struct evlist *evlist);
void evlist__close(struct evlist *evlist);

struct callchain_param;

void perf_evlist__set_id_pos(struct evlist *evlist);
bool perf_can_sample_identifier(void);
bool perf_can_record_switch_events(void);
bool perf_can_record_cpu_wide(void);
void perf_evlist__config(struct evlist *evlist, struct record_opts *opts,
			 struct callchain_param *callchain);
int record_opts__config(struct record_opts *opts);

int perf_evlist__prepare_workload(struct evlist *evlist,
				  struct target *target,
				  const char *argv[], bool pipe_output,
				  void (*exec_error)(int signo, siginfo_t *info,
						     void *ucontext));
int perf_evlist__start_workload(struct evlist *evlist);

struct option;

int __perf_evlist__parse_mmap_pages(unsigned int *mmap_pages, const char *str);
int perf_evlist__parse_mmap_pages(const struct option *opt,
				  const char *str,
				  int unset);

unsigned long perf_event_mlock_kb_in_pages(void);

int evlist__mmap_ex(struct evlist *evlist, unsigned int pages,
			 unsigned int auxtrace_pages,
			 bool auxtrace_overwrite, int nr_cblocks,
			 int affinity, int flush, int comp_level);
int evlist__mmap(struct evlist *evlist, unsigned int pages);
void evlist__munmap(struct evlist *evlist);

size_t evlist__mmap_size(unsigned long pages);

void evlist__disable(struct evlist *evlist);
void evlist__enable(struct evlist *evlist);
void perf_evlist__toggle_enable(struct evlist *evlist);

int perf_evlist__enable_event_idx(struct evlist *evlist,
				  struct evsel *evsel, int idx);

void perf_evlist__set_selected(struct evlist *evlist,
			       struct evsel *evsel);

int perf_evlist__create_maps(struct evlist *evlist, struct target *target);
int perf_evlist__apply_filters(struct evlist *evlist, struct evsel **err_evsel);

void __perf_evlist__set_leader(struct list_head *list);
void perf_evlist__set_leader(struct evlist *evlist);

u64 __perf_evlist__combined_sample_type(struct evlist *evlist);
u64 perf_evlist__combined_sample_type(struct evlist *evlist);
u64 perf_evlist__combined_branch_type(struct evlist *evlist);
bool perf_evlist__sample_id_all(struct evlist *evlist);
u16 perf_evlist__id_hdr_size(struct evlist *evlist);

int perf_evlist__parse_sample(struct evlist *evlist, union perf_event *event,
			      struct perf_sample *sample);

int perf_evlist__parse_sample_timestamp(struct evlist *evlist,
					union perf_event *event,
					u64 *timestamp);

bool perf_evlist__valid_sample_type(struct evlist *evlist);
bool perf_evlist__valid_sample_id_all(struct evlist *evlist);
bool perf_evlist__valid_read_format(struct evlist *evlist);

void perf_evlist__splice_list_tail(struct evlist *evlist,
				   struct list_head *list);

static inline bool perf_evlist__empty(struct evlist *evlist)
{
	return list_empty(&evlist->core.entries);
}

static inline struct evsel *evlist__first(struct evlist *evlist)
{
	struct perf_evsel *evsel = perf_evlist__first(&evlist->core);

	return container_of(evsel, struct evsel, core);
}

static inline struct evsel *evlist__last(struct evlist *evlist)
{
	struct perf_evsel *evsel = perf_evlist__last(&evlist->core);

	return container_of(evsel, struct evsel, core);
}

int perf_evlist__strerror_open(struct evlist *evlist, int err, char *buf, size_t size);
int perf_evlist__strerror_mmap(struct evlist *evlist, int err, char *buf, size_t size);

bool perf_evlist__can_select_event(struct evlist *evlist, const char *str);
void perf_evlist__to_front(struct evlist *evlist,
			   struct evsel *move_evsel);

/**
 * __evlist__for_each_entry - iterate thru all the evsels
 * @list: list_head instance to iterate
 * @evsel: struct evsel iterator
 */
#define __evlist__for_each_entry(list, evsel) \
        list_for_each_entry(evsel, list, core.node)

/**
 * evlist__for_each_entry - iterate thru all the evsels
 * @evlist: evlist instance to iterate
 * @evsel: struct evsel iterator
 */
#define evlist__for_each_entry(evlist, evsel) \
	__evlist__for_each_entry(&(evlist)->core.entries, evsel)

/**
 * __evlist__for_each_entry_continue - continue iteration thru all the evsels
 * @list: list_head instance to iterate
 * @evsel: struct evsel iterator
 */
#define __evlist__for_each_entry_continue(list, evsel) \
        list_for_each_entry_continue(evsel, list, core.node)

/**
 * evlist__for_each_entry_continue - continue iteration thru all the evsels
 * @evlist: evlist instance to iterate
 * @evsel: struct evsel iterator
 */
#define evlist__for_each_entry_continue(evlist, evsel) \
	__evlist__for_each_entry_continue(&(evlist)->core.entries, evsel)

/**
 * __evlist__for_each_entry_reverse - iterate thru all the evsels in reverse order
 * @list: list_head instance to iterate
 * @evsel: struct evsel iterator
 */
#define __evlist__for_each_entry_reverse(list, evsel) \
        list_for_each_entry_reverse(evsel, list, core.node)

/**
 * evlist__for_each_entry_reverse - iterate thru all the evsels in reverse order
 * @evlist: evlist instance to iterate
 * @evsel: struct evsel iterator
 */
#define evlist__for_each_entry_reverse(evlist, evsel) \
	__evlist__for_each_entry_reverse(&(evlist)->core.entries, evsel)

/**
 * __evlist__for_each_entry_safe - safely iterate thru all the evsels
 * @list: list_head instance to iterate
 * @tmp: struct evsel temp iterator
 * @evsel: struct evsel iterator
 */
#define __evlist__for_each_entry_safe(list, tmp, evsel) \
        list_for_each_entry_safe(evsel, tmp, list, core.node)

/**
 * evlist__for_each_entry_safe - safely iterate thru all the evsels
 * @evlist: evlist instance to iterate
 * @evsel: struct evsel iterator
 * @tmp: struct evsel temp iterator
 */
#define evlist__for_each_entry_safe(evlist, tmp, evsel) \
	__evlist__for_each_entry_safe(&(evlist)->core.entries, tmp, evsel)

void perf_evlist__set_tracking_event(struct evlist *evlist,
				     struct evsel *tracking_evsel);

struct evsel *
perf_evlist__find_evsel_by_str(struct evlist *evlist, const char *str);

struct evsel *perf_evlist__event2evsel(struct evlist *evlist,
					    union perf_event *event);

bool perf_evlist__exclude_kernel(struct evlist *evlist);

void perf_evlist__force_leader(struct evlist *evlist);

struct evsel *perf_evlist__reset_weak_group(struct evlist *evlist,
						 struct evsel *evsel);
#endif /* __PERF_EVLIST_H */
