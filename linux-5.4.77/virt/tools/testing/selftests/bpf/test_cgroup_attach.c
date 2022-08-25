// SPDX-License-Identifier: GPL-2.0

/* eBPF example program:
 *
 * - Creates arraymap in kernel with 4 bytes keys and 8 byte values
 *
 * - Loads eBPF program
 *
 *   The eBPF program accesses the map passed in to store two pieces of
 *   information. The number of invocations of the program, which maps
 *   to the number of packets received, is stored to key 0. Key 1 is
 *   incremented on each iteration by the number of bytes stored in
 *   the skb. The program also stores the number of received bytes
 *   in the cgroup storage.
 *
 * - Attaches the new program to a cgroup using BPF_PROG_ATTACH
 *
 * - Every second, reads map[0] and map[1] to see how many bytes and
 *   packets were seen on any socket of tasks in the given cgroup.
 */

#define _GNU_SOURCE

#include <stdio.h>
#include <stdlib.h>
#include <assert.h>
#include <sys/resource.h>
#include <sys/time.h>
#include <unistd.h>
#include <linux/filter.h>

#include <linux/bpf.h>
#include <bpf/bpf.h>

#include "bpf_util.h"
#include "bpf_rlimit.h"
#include "cgroup_helpers.h"

#define FOO		"/foo"
#define BAR		"/foo/bar/"
#define PING_CMD	"ping -q -c1 -w1 127.0.0.1 > /dev/null"

char bpf_log_buf[BPF_LOG_BUF_SIZE];

#ifdef DEBUG
#define debug(args...) printf(args)
#else
#define debug(args...)
#endif

static int prog_load(int verdict)
{
	int ret;
	struct bpf_insn prog[] = {
		BPF_MOV64_IMM(BPF_REG_0, verdict), /* r0 = verdict */
		BPF_EXIT_INSN(),
	};
	size_t insns_cnt = sizeof(prog) / sizeof(struct bpf_insn);

	ret = bpf_load_program(BPF_PROG_TYPE_CGROUP_SKB,
			       prog, insns_cnt, "GPL", 0,
			       bpf_log_buf, BPF_LOG_BUF_SIZE);

	if (ret < 0) {
		log_err("Loading program");
		printf("Output from verifier:\n%s\n-------\n", bpf_log_buf);
		return 0;
	}
	return ret;
}

static int test_foo_bar(void)
{
	int drop_prog, allow_prog, foo = 0, bar = 0, rc = 0;

	allow_prog = prog_load(1);
	if (!allow_prog)
		goto err;

	drop_prog = prog_load(0);
	if (!drop_prog)
		goto err;

	if (setup_cgroup_environment())
		goto err;

	/* Create cgroup /foo, get fd, and join it */
	foo = create_and_get_cgroup(FOO);
	if (foo < 0)
		goto err;

	if (join_cgroup(FOO))
		goto err;

	if (bpf_prog_attach(drop_prog, foo, BPF_CGROUP_INET_EGRESS,
			    BPF_F_ALLOW_OVERRIDE)) {
		log_err("Attaching prog to /foo");
		goto err;
	}

	debug("Attached DROP prog. This ping in cgroup /foo should fail...\n");
	assert(system(PING_CMD) != 0);

	/* Create cgroup /foo/bar, get fd, and join it */
	bar = create_and_get_cgroup(BAR);
	if (bar < 0)
		goto err;

	if (join_cgroup(BAR))
		goto err;

	debug("Attached DROP prog. This ping in cgroup /foo/bar should fail...\n");
	assert(system(PING_CMD) != 0);

	if (bpf_prog_attach(allow_prog, bar, BPF_CGROUP_INET_EGRESS,
			    BPF_F_ALLOW_OVERRIDE)) {
		log_err("Attaching prog to /foo/bar");
		goto err;
	}

	debug("Attached PASS prog. This ping in cgroup /foo/bar should pass...\n");
	assert(system(PING_CMD) == 0);

	if (bpf_prog_detach(bar, BPF_CGROUP_INET_EGRESS)) {
		log_err("Detaching program from /foo/bar");
		goto err;
	}

	debug("Detached PASS from /foo/bar while DROP is attached to /foo.\n"
	       "This ping in cgroup /foo/bar should fail...\n");
	assert(system(PING_CMD) != 0);

	if (bpf_prog_attach(allow_prog, bar, BPF_CGROUP_INET_EGRESS,
			    BPF_F_ALLOW_OVERRIDE)) {
		log_err("Attaching prog to /foo/bar");
		goto err;
	}

	if (bpf_prog_detach(foo, BPF_CGROUP_INET_EGRESS)) {
		log_err("Detaching program from /foo");
		goto err;
	}

	debug("Attached PASS from /foo/bar and detached DROP from /foo.\n"
	       "This ping in cgroup /foo/bar should pass...\n");
	assert(system(PING_CMD) == 0);

	if (bpf_prog_attach(allow_prog, bar, BPF_CGROUP_INET_EGRESS,
			    BPF_F_ALLOW_OVERRIDE)) {
		log_err("Attaching prog to /foo/bar");
		goto err;
	}

	if (!bpf_prog_attach(allow_prog, bar, BPF_CGROUP_INET_EGRESS, 0)) {
		errno = 0;
		log_err("Unexpected success attaching prog to /foo/bar");
		goto err;
	}

	if (bpf_prog_detach(bar, BPF_CGROUP_INET_EGRESS)) {
		log_err("Detaching program from /foo/bar");
		goto err;
	}

	if (!bpf_prog_detach(foo, BPF_CGROUP_INET_EGRESS)) {
		errno = 0;
		log_err("Unexpected success in double detach from /foo");
		goto err;
	}

	if (bpf_prog_attach(allow_prog, foo, BPF_CGROUP_INET_EGRESS, 0)) {
		log_err("Attaching non-overridable prog to /foo");
		goto err;
	}

	if (!bpf_prog_attach(allow_prog, bar, BPF_CGROUP_INET_EGRESS, 0)) {
		errno = 0;
		log_err("Unexpected success attaching non-overridable prog to /foo/bar");
		goto err;
	}

	if (!bpf_prog_attach(allow_prog, bar, BPF_CGROUP_INET_EGRESS,
			     BPF_F_ALLOW_OVERRIDE)) {
		errno = 0;
		log_err("Unexpected success attaching overridable prog to /foo/bar");
		goto err;
	}

	if (!bpf_prog_attach(allow_prog, foo, BPF_CGROUP_INET_EGRESS,
			     BPF_F_ALLOW_OVERRIDE)) {
		errno = 0;
		log_err("Unexpected success attaching overridable prog to /foo");
		goto err;
	}

	if (bpf_prog_attach(drop_prog, foo, BPF_CGROUP_INET_EGRESS, 0)) {
		log_err("Attaching different non-overridable prog to /foo");
		goto err;
	}

	goto out;

err:
	rc = 1;

out:
	close(foo);
	close(bar);
	cleanup_cgroup_environment();
	if (!rc)
		printf("#override:PASS\n");
	else
		printf("#override:FAIL\n");
	return rc;
}

static int map_fd = -1;

static int prog_load_cnt(int verdict, int val)
{
	int cgroup_storage_fd, percpu_cgroup_storage_fd;

	if (map_fd < 0)
		map_fd = bpf_create_map(BPF_MAP_TYPE_ARRAY, 4, 8, 1, 0);
	if (map_fd < 0) {
		printf("failed to create map '%s'\n", strerror(errno));
		return -1;
	}

	cgroup_storage_fd = bpf_create_map(BPF_MAP_TYPE_CGROUP_STORAGE,
				sizeof(struct bpf_cgroup_storage_key), 8, 0, 0);
	if (cgroup_storage_fd < 0) {
		printf("failed to create map '%s'\n", strerror(errno));
		return -1;
	}

	percpu_cgroup_storage_fd = bpf_create_map(
		BPF_MAP_TYPE_PERCPU_CGROUP_STORAGE,
		sizeof(struct bpf_cgroup_storage_key), 8, 0, 0);
	if (percpu_cgroup_storage_fd < 0) {
		printf("failed to create map '%s'\n", strerror(errno));
		return -1;
	}

	struct bpf_insn prog[] = {
		BPF_MOV32_IMM(BPF_REG_0, 0),
		BPF_STX_MEM(BPF_W, BPF_REG_10, BPF_REG_0, -4), /* *(u32 *)(fp - 4) = r0 */
		BPF_MOV64_REG(BPF_REG_2, BPF_REG_10),
		BPF_ALU64_IMM(BPF_ADD, BPF_REG_2, -4), /* r2 = fp - 4 */
		BPF_LD_MAP_FD(BPF_REG_1, map_fd),
		BPF_RAW_INSN(BPF_JMP | BPF_CALL, 0, 0, 0, BPF_FUNC_map_lookup_elem),
		BPF_JMP_IMM(BPF_JEQ, BPF_REG_0, 0, 2),
		BPF_MOV64_IMM(BPF_REG_1, val), /* r1 = 1 */
		BPF_RAW_INSN(BPF_STX | BPF_XADD | BPF_DW, BPF_REG_0, BPF_REG_1, 0, 0), /* xadd r0 += r1 */

		BPF_LD_MAP_FD(BPF_REG_1, cgroup_storage_fd),
		BPF_MOV64_IMM(BPF_REG_2, 0),
		BPF_RAW_INSN(BPF_JMP | BPF_CALL, 0, 0, 0, BPF_FUNC_get_local_storage),
		BPF_MOV64_IMM(BPF_REG_1, val),
		BPF_RAW_INSN(BPF_STX | BPF_XADD | BPF_W, BPF_REG_0, BPF_REG_1, 0, 0),

		BPF_LD_MAP_FD(BPF_REG_1, percpu_cgroup_storage_fd),
		BPF_MOV64_IMM(BPF_REG_2, 0),
		BPF_RAW_INSN(BPF_JMP | BPF_CALL, 0, 0, 0, BPF_FUNC_get_local_storage),
		BPF_LDX_MEM(BPF_W, BPF_REG_3, BPF_REG_0, 0),
		BPF_ALU64_IMM(BPF_ADD, BPF_REG_3, 0x1),
		BPF_STX_MEM(BPF_W, BPF_REG_0, BPF_REG_3, 0),

		BPF_MOV64_IMM(BPF_REG_0, verdict), /* r0 = verdict */
		BPF_EXIT_INSN(),
	};
	size_t insns_cnt = sizeof(prog) / sizeof(struct bpf_insn);
	int ret;

	ret = bpf_load_program(BPF_PROG_TYPE_CGROUP_SKB,
			       prog, insns_cnt, "GPL", 0,
			       bpf_log_buf, BPF_LOG_BUF_SIZE);

	if (ret < 0) {
		log_err("Loading program");
		printf("Output from verifier:\n%s\n-------\n", bpf_log_buf);
		return 0;
	}
	close(cgroup_storage_fd);
	return ret;
}


static int test_multiprog(void)
{
	__u32 prog_ids[4], prog_cnt = 0, attach_flags, saved_prog_id;
	int cg1 = 0, cg2 = 0, cg3 = 0, cg4 = 0, cg5 = 0, key = 0;
	int drop_prog, allow_prog[6] = {}, rc = 0;
	unsigned long long value;
	int i = 0;

	for (i = 0; i < 6; i++) {
		allow_prog[i] = prog_load_cnt(1, 1 << i);
		if (!allow_prog[i])
			goto err;
	}
	drop_prog = prog_load_cnt(0, 1);
	if (!drop_prog)
		goto err;

	if (setup_cgroup_environment())
		goto err;

	cg1 = create_and_get_cgroup("/cg1");
	if (cg1 < 0)
		goto err;
	cg2 = create_and_get_cgroup("/cg1/cg2");
	if (cg2 < 0)
		goto err;
	cg3 = create_and_get_cgroup("/cg1/cg2/cg3");
	if (cg3 < 0)
		goto err;
	cg4 = create_and_get_cgroup("/cg1/cg2/cg3/cg4");
	if (cg4 < 0)
		goto err;
	cg5 = create_and_get_cgroup("/cg1/cg2/cg3/cg4/cg5");
	if (cg5 < 0)
		goto err;

	if (join_cgroup("/cg1/cg2/cg3/cg4/cg5"))
		goto err;

	if (bpf_prog_attach(allow_prog[0], cg1, BPF_CGROUP_INET_EGRESS,
			    BPF_F_ALLOW_MULTI)) {
		log_err("Attaching prog to cg1");
		goto err;
	}
	if (!bpf_prog_attach(allow_prog[0], cg1, BPF_CGROUP_INET_EGRESS,
			     BPF_F_ALLOW_MULTI)) {
		log_err("Unexpected success attaching the same prog to cg1");
		goto err;
	}
	if (bpf_prog_attach(allow_prog[1], cg1, BPF_CGROUP_INET_EGRESS,
			    BPF_F_ALLOW_MULTI)) {
		log_err("Attaching prog2 to cg1");
		goto err;
	}
	if (bpf_prog_attach(allow_prog[2], cg2, BPF_CGROUP_INET_EGRESS,
			    BPF_F_ALLOW_OVERRIDE)) {
		log_err("Attaching prog to cg2");
		goto err;
	}
	if (bpf_prog_attach(allow_prog[3], cg3, BPF_CGROUP_INET_EGRESS,
			    BPF_F_ALLOW_MULTI)) {
		log_err("Attaching prog to cg3");
		goto err;
	}
	if (bpf_prog_attach(allow_prog[4], cg4, BPF_CGROUP_INET_EGRESS,
			    BPF_F_ALLOW_OVERRIDE)) {
		log_err("Attaching prog to cg4");
		goto err;
	}
	if (bpf_prog_attach(allow_prog[5], cg5, BPF_CGROUP_INET_EGRESS, 0)) {
		log_err("Attaching prog to cg5");
		goto err;
	}
	assert(system(PING_CMD) == 0);
	assert(bpf_map_lookup_elem(map_fd, &key, &value) == 0);
	assert(value == 1 + 2 + 8 + 32);

	/* query the number of effective progs in cg5 */
	assert(bpf_prog_query(cg5, BPF_CGROUP_INET_EGRESS, BPF_F_QUERY_EFFECTIVE,
			      NULL, NULL, &prog_cnt) == 0);
	assert(prog_cnt == 4);
	/* retrieve prog_ids of effective progs in cg5 */
	assert(bpf_prog_query(cg5, BPF_CGROUP_INET_EGRESS, BPF_F_QUERY_EFFECTIVE,
			      &attach_flags, prog_ids, &prog_cnt) == 0);
	assert(prog_cnt == 4);
	assert(attach_flags == 0);
	saved_prog_id = prog_ids[0];
	/* check enospc handling */
	prog_ids[0] = 0;
	prog_cnt = 2;
	assert(bpf_prog_query(cg5, BPF_CGROUP_INET_EGRESS, BPF_F_QUERY_EFFECTIVE,
			      &attach_flags, prog_ids, &prog_cnt) == -1 &&
	       errno == ENOSPC);
	assert(prog_cnt == 4);
	/* check that prog_ids are returned even when buffer is too small */
	assert(prog_ids[0] == saved_prog_id);
	/* retrieve prog_id of single attached prog in cg5 */
	prog_ids[0] = 0;
	assert(bpf_prog_query(cg5, BPF_CGROUP_INET_EGRESS, 0,
			      NULL, prog_ids, &prog_cnt) == 0);
	assert(prog_cnt == 1);
	assert(prog_ids[0] == saved_prog_id);

	/* detach bottom program and ping again */
	if (bpf_prog_detach2(-1, cg5, BPF_CGROUP_INET_EGRESS)) {
		log_err("Detaching prog from cg5");
		goto err;
	}
	value = 0;
	assert(bpf_map_update_elem(map_fd, &key, &value, 0) == 0);
	assert(system(PING_CMD) == 0);
	assert(bpf_map_lookup_elem(map_fd, &key, &value) == 0);
	assert(value == 1 + 2 + 8 + 16);

	/* detach 3rd from bottom program and ping again */
	errno = 0;
	if (!bpf_prog_detach2(0, cg3, BPF_CGROUP_INET_EGRESS)) {
		log_err("Unexpected success on detach from cg3");
		goto err;
	}
	if (bpf_prog_detach2(allow_prog[3], cg3, BPF_CGROUP_INET_EGRESS)) {
		log_err("Detaching from cg3");
		goto err;
	}
	value = 0;
	assert(bpf_map_update_elem(map_fd, &key, &value, 0) == 0);
	assert(system(PING_CMD) == 0);
	assert(bpf_map_lookup_elem(map_fd, &key, &value) == 0);
	assert(value == 1 + 2 + 16);

	/* detach 2nd from bottom program and ping again */
	if (bpf_prog_detach2(-1, cg4, BPF_CGROUP_INET_EGRESS)) {
		log_err("Detaching prog from cg4");
		goto err;
	}
	value = 0;
	assert(bpf_map_update_elem(map_fd, &key, &value, 0) == 0);
	assert(system(PING_CMD) == 0);
	assert(bpf_map_lookup_elem(map_fd, &key, &value) == 0);
	assert(value == 1 + 2 + 4);

	prog_cnt = 4;
	assert(bpf_prog_query(cg5, BPF_CGROUP_INET_EGRESS, BPF_F_QUERY_EFFECTIVE,
			      &attach_flags, prog_ids, &prog_cnt) == 0);
	assert(prog_cnt == 3);
	assert(attach_flags == 0);
	assert(bpf_prog_query(cg5, BPF_CGROUP_INET_EGRESS, 0,
			      NULL, prog_ids, &prog_cnt) == 0);
	assert(prog_cnt == 0);
	goto out;
err:
	rc = 1;

out:
	for (i = 0; i < 6; i++)
		if (allow_prog[i] > 0)
			close(allow_prog[i]);
	close(cg1);
	close(cg2);
	close(cg3);
	close(cg4);
	close(cg5);
	cleanup_cgroup_environment();
	if (!rc)
		printf("#multi:PASS\n");
	else
		printf("#multi:FAIL\n");
	return rc;
}

static int test_autodetach(void)
{
	__u32 prog_cnt = 4, attach_flags;
	int allow_prog[2] = {0};
	__u32 prog_ids[2] = {0};
	int cg = 0, i, rc = -1;
	void *ptr = NULL;
	int attempts;

	for (i = 0; i < ARRAY_SIZE(allow_prog); i++) {
		allow_prog[i] = prog_load_cnt(1, 1 << i);
		if (!allow_prog[i])
			goto err;
	}

	if (setup_cgroup_environment())
		goto err;

	/* create a cgroup, attach two programs and remember their ids */
	cg = create_and_get_cgroup("/cg_autodetach");
	if (cg < 0)
		goto err;

	if (join_cgroup("/cg_autodetach"))
		goto err;

	for (i = 0; i < ARRAY_SIZE(allow_prog); i++) {
		if (bpf_prog_attach(allow_prog[i], cg, BPF_CGROUP_INET_EGRESS,
				    BPF_F_ALLOW_MULTI)) {
			log_err("Attaching prog[%d] to cg:egress", i);
			goto err;
		}
	}

	/* make sure that programs are attached and run some traffic */
	assert(bpf_prog_query(cg, BPF_CGROUP_INET_EGRESS, 0, &attach_flags,
			      prog_ids, &prog_cnt) == 0);
	assert(system(PING_CMD) == 0);

	/* allocate some memory (4Mb) to pin the original cgroup */
	ptr = malloc(4 * (1 << 20));
	if (!ptr)
		goto err;

	/* close programs and cgroup fd */
	for (i = 0; i < ARRAY_SIZE(allow_prog); i++) {
		close(allow_prog[i]);
		allow_prog[i] = 0;
	}

	close(cg);
	cg = 0;

	/* leave the cgroup and remove it. don't detach programs */
	cleanup_cgroup_environment();

	/* wait for the asynchronous auto-detachment.
	 * wait for no more than 5 sec and give up.
	 */
	for (i = 0; i < ARRAY_SIZE(prog_ids); i++) {
		for (attempts = 5; attempts >= 0; attempts--) {
			int fd = bpf_prog_get_fd_by_id(prog_ids[i]);

			if (fd < 0)
				break;

			/* don't leave the fd open */
			close(fd);

			if (!attempts)
				goto err;

			sleep(1);
		}
	}

	rc = 0;
err:
	for (i = 0; i < ARRAY_SIZE(allow_prog); i++)
		if (allow_prog[i] > 0)
			close(allow_prog[i]);
	if (cg)
		close(cg);
	free(ptr);
	cleanup_cgroup_environment();
	if (!rc)
		printf("#autodetach:PASS\n");
	else
		printf("#autodetach:FAIL\n");
	return rc;
}

int main(void)
{
	int (*tests[])(void) = {
		test_foo_bar,
		test_multiprog,
		test_autodetach,
	};
	int errors = 0;
	int i;

	for (i = 0; i < ARRAY_SIZE(tests); i++)
		if (tests[i]())
			errors++;

	if (errors)
		printf("test_cgroup_attach:FAIL\n");
	else
		printf("test_cgroup_attach:PASS\n");

	return errors ? EXIT_FAILURE : EXIT_SUCCESS;
}
