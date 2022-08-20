// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright 2018 Google Inc.
 * Author: Eric Dumazet (edumazet@google.com)
 *
 * Reference program demonstrating tcp mmap() usage,
 * and SO_RCVLOWAT hints for receiver.
 *
 * Note : NIC with header split is needed to use mmap() on TCP :
 * Each incoming frame must be a multiple of PAGE_SIZE bytes of TCP payload.
 *
 * How to use on loopback interface :
 *
 *  ifconfig lo mtu 61512  # 15*4096 + 40 (ipv6 header) + 32 (TCP with TS option header)
 *  tcp_mmap -s -z &
 *  tcp_mmap -H ::1 -z
 *
 *  Or leave default lo mtu, but use -M option to set TCP_MAXSEG option to (4096 + 12)
 *      (4096 : page size on x86, 12: TCP TS option length)
 *  tcp_mmap -s -z -M $((4096+12)) &
 *  tcp_mmap -H ::1 -z -M $((4096+12))
 *
 * Note: -z option on sender uses MSG_ZEROCOPY, which forces a copy when packets go through loopback interface.
 *       We might use sendfile() instead, but really this test program is about mmap(), for receivers ;)
 *
 * $ ./tcp_mmap -s &                                 # Without mmap()
 * $ for i in {1..4}; do ./tcp_mmap -H ::1 -z ; done
 * received 32768 MB (0 % mmap'ed) in 14.1157 s, 19.4732 Gbit
 *   cpu usage user:0.057 sys:7.815, 240.234 usec per MB, 65531 c-switches
 * received 32768 MB (0 % mmap'ed) in 14.6833 s, 18.7204 Gbit
 *  cpu usage user:0.043 sys:8.103, 248.596 usec per MB, 65524 c-switches
 * received 32768 MB (0 % mmap'ed) in 11.143 s, 24.6682 Gbit
 *   cpu usage user:0.044 sys:6.576, 202.026 usec per MB, 65519 c-switches
 * received 32768 MB (0 % mmap'ed) in 14.9056 s, 18.4413 Gbit
 *   cpu usage user:0.036 sys:8.193, 251.129 usec per MB, 65530 c-switches
 * $ kill %1   # kill tcp_mmap server
 *
 * $ ./tcp_mmap -s -z &                              # With mmap()
 * $ for i in {1..4}; do ./tcp_mmap -H ::1 -z ; done
 * received 32768 MB (99.9939 % mmap'ed) in 6.73792 s, 40.7956 Gbit
 *   cpu usage user:0.045 sys:2.827, 87.6465 usec per MB, 65532 c-switches
 * received 32768 MB (99.9939 % mmap'ed) in 7.26732 s, 37.8238 Gbit
 *   cpu usage user:0.037 sys:3.087, 95.3369 usec per MB, 65532 c-switches
 * received 32768 MB (99.9939 % mmap'ed) in 7.61661 s, 36.0893 Gbit
 *   cpu usage user:0.046 sys:3.559, 110.016 usec per MB, 65529 c-switches
 * received 32768 MB (99.9939 % mmap'ed) in 7.43764 s, 36.9577 Gbit
 *   cpu usage user:0.035 sys:3.467, 106.873 usec per MB, 65530 c-switches
 */
#define _GNU_SOURCE
#include <pthread.h>
#include <sys/types.h>
#include <fcntl.h>
#include <error.h>
#include <sys/socket.h>
#include <sys/mman.h>
#include <sys/resource.h>
#include <unistd.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <errno.h>
#include <time.h>
#include <sys/time.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <poll.h>
#include <linux/tcp.h>
#include <assert.h>

#ifndef MSG_ZEROCOPY
#define MSG_ZEROCOPY    0x4000000
#endif

#define FILE_SZ (1UL << 35)
static int cfg_family = AF_INET6;
static socklen_t cfg_alen = sizeof(struct sockaddr_in6);
static int cfg_port = 8787;

static int rcvbuf; /* Default: autotuning.  Can be set with -r <integer> option */
static int sndbuf; /* Default: autotuning.  Can be set with -w <integer> option */
static int zflg; /* zero copy option. (MSG_ZEROCOPY for sender, mmap() for receiver */
static int xflg; /* hash received data (simple xor) (-h option) */
static int keepflag; /* -k option: receiver shall keep all received file in memory (no munmap() calls) */

static int chunk_size  = 512*1024;

unsigned long htotal;

static inline void prefetch(const void *x)
{
#if defined(__x86_64__)
	asm volatile("prefetcht0 %P0" : : "m" (*(const char *)x));
#endif
}

void hash_zone(void *zone, unsigned int length)
{
	unsigned long temp = htotal;

	while (length >= 8*sizeof(long)) {
		prefetch(zone + 384);
		temp ^= *(unsigned long *)zone;
		temp ^= *(unsigned long *)(zone + sizeof(long));
		temp ^= *(unsigned long *)(zone + 2*sizeof(long));
		temp ^= *(unsigned long *)(zone + 3*sizeof(long));
		temp ^= *(unsigned long *)(zone + 4*sizeof(long));
		temp ^= *(unsigned long *)(zone + 5*sizeof(long));
		temp ^= *(unsigned long *)(zone + 6*sizeof(long));
		temp ^= *(unsigned long *)(zone + 7*sizeof(long));
		zone += 8*sizeof(long);
		length -= 8*sizeof(long);
	}
	while (length >= 1) {
		temp ^= *(unsigned char *)zone;
		zone += 1;
		length--;
	}
	htotal = temp;
}

void *child_thread(void *arg)
{
	unsigned long total_mmap = 0, total = 0;
	struct tcp_zerocopy_receive zc;
	unsigned long delta_usec;
	int flags = MAP_SHARED;
	struct timeval t0, t1;
	char *buffer = NULL;
	void *addr = NULL;
	double throughput;
	struct rusage ru;
	int lu, fd;

	fd = (int)(unsigned long)arg;

	gettimeofday(&t0, NULL);

	fcntl(fd, F_SETFL, O_NDELAY);
	buffer = malloc(chunk_size);
	if (!buffer) {
		perror("malloc");
		goto error;
	}
	if (zflg) {
		addr = mmap(NULL, chunk_size, PROT_READ, flags, fd, 0);
		if (addr == (void *)-1)
			zflg = 0;
	}
	while (1) {
		struct pollfd pfd = { .fd = fd, .events = POLLIN, };
		int sub;

		poll(&pfd, 1, 10000);
		if (zflg) {
			socklen_t zc_len = sizeof(zc);
			int res;

			zc.address = (__u64)addr;
			zc.length = chunk_size;
			zc.recv_skip_hint = 0;
			res = getsockopt(fd, IPPROTO_TCP, TCP_ZEROCOPY_RECEIVE,
					 &zc, &zc_len);
			if (res == -1)
				break;

			if (zc.length) {
				assert(zc.length <= chunk_size);
				total_mmap += zc.length;
				if (xflg)
					hash_zone(addr, zc.length);
				total += zc.length;
			}
			if (zc.recv_skip_hint) {
				assert(zc.recv_skip_hint <= chunk_size);
				lu = read(fd, buffer, zc.recv_skip_hint);
				if (lu > 0) {
					if (xflg)
						hash_zone(buffer, lu);
					total += lu;
				}
			}
			continue;
		}
		sub = 0;
		while (sub < chunk_size) {
			lu = read(fd, buffer + sub, chunk_size - sub);
			if (lu == 0)
				goto end;
			if (lu < 0)
				break;
			if (xflg)
				hash_zone(buffer + sub, lu);
			total += lu;
			sub += lu;
		}
	}
end:
	gettimeofday(&t1, NULL);
	delta_usec = (t1.tv_sec - t0.tv_sec) * 1000000 + t1.tv_usec - t0.tv_usec;

	throughput = 0;
	if (delta_usec)
		throughput = total * 8.0 / (double)delta_usec / 1000.0;
	getrusage(RUSAGE_THREAD, &ru);
	if (total > 1024*1024) {
		unsigned long total_usec;
		unsigned long mb = total >> 20;
		total_usec = 1000000*ru.ru_utime.tv_sec + ru.ru_utime.tv_usec +
			     1000000*ru.ru_stime.tv_sec + ru.ru_stime.tv_usec;
		printf("received %lg MB (%lg %% mmap'ed) in %lg s, %lg Gbit\n"
		       "  cpu usage user:%lg sys:%lg, %lg usec per MB, %lu c-switches\n",
				total / (1024.0 * 1024.0),
				100.0*total_mmap/total,
				(double)delta_usec / 1000000.0,
				throughput,
				(double)ru.ru_utime.tv_sec + (double)ru.ru_utime.tv_usec / 1000000.0,
				(double)ru.ru_stime.tv_sec + (double)ru.ru_stime.tv_usec / 1000000.0,
				(double)total_usec/mb,
				ru.ru_nvcsw);
	}
error:
	free(buffer);
	close(fd);
	if (zflg)
		munmap(addr, chunk_size);
	pthread_exit(0);
}

static void apply_rcvsnd_buf(int fd)
{
	if (rcvbuf && setsockopt(fd, SOL_SOCKET,
				 SO_RCVBUF, &rcvbuf, sizeof(rcvbuf)) == -1) {
		perror("setsockopt SO_RCVBUF");
	}

	if (sndbuf && setsockopt(fd, SOL_SOCKET,
				 SO_SNDBUF, &sndbuf, sizeof(sndbuf)) == -1) {
		perror("setsockopt SO_SNDBUF");
	}
}


static void setup_sockaddr(int domain, const char *str_addr,
			   struct sockaddr_storage *sockaddr)
{
	struct sockaddr_in6 *addr6 = (void *) sockaddr;
	struct sockaddr_in *addr4 = (void *) sockaddr;

	switch (domain) {
	case PF_INET:
		memset(addr4, 0, sizeof(*addr4));
		addr4->sin_family = AF_INET;
		addr4->sin_port = htons(cfg_port);
		if (str_addr &&
		    inet_pton(AF_INET, str_addr, &(addr4->sin_addr)) != 1)
			error(1, 0, "ipv4 parse error: %s", str_addr);
		break;
	case PF_INET6:
		memset(addr6, 0, sizeof(*addr6));
		addr6->sin6_family = AF_INET6;
		addr6->sin6_port = htons(cfg_port);
		if (str_addr &&
		    inet_pton(AF_INET6, str_addr, &(addr6->sin6_addr)) != 1)
			error(1, 0, "ipv6 parse error: %s", str_addr);
		break;
	default:
		error(1, 0, "illegal domain");
	}
}

static void do_accept(int fdlisten)
{
	if (setsockopt(fdlisten, SOL_SOCKET, SO_RCVLOWAT,
		       &chunk_size, sizeof(chunk_size)) == -1) {
		perror("setsockopt SO_RCVLOWAT");
	}

	apply_rcvsnd_buf(fdlisten);

	while (1) {
		struct sockaddr_in addr;
		socklen_t addrlen = sizeof(addr);
		pthread_t th;
		int fd, res;

		fd = accept(fdlisten, (struct sockaddr *)&addr, &addrlen);
		if (fd == -1) {
			perror("accept");
			continue;
		}
		res = pthread_create(&th, NULL, child_thread,
				     (void *)(unsigned long)fd);
		if (res) {
			errno = res;
			perror("pthread_create");
			close(fd);
		}
	}
}

int main(int argc, char *argv[])
{
	struct sockaddr_storage listenaddr, addr;
	unsigned int max_pacing_rate = 0;
	unsigned long total = 0;
	char *host = NULL;
	int fd, c, on = 1;
	char *buffer;
	int sflg = 0;
	int mss = 0;

	while ((c = getopt(argc, argv, "46p:svr:w:H:zxkP:M:")) != -1) {
		switch (c) {
		case '4':
			cfg_family = PF_INET;
			cfg_alen = sizeof(struct sockaddr_in);
			break;
		case '6':
			cfg_family = PF_INET6;
			cfg_alen = sizeof(struct sockaddr_in6);
			break;
		case 'p':
			cfg_port = atoi(optarg);
			break;
		case 'H':
			host = optarg;
			break;
		case 's': /* server : listen for incoming connections */
			sflg++;
			break;
		case 'r':
			rcvbuf = atoi(optarg);
			break;
		case 'w':
			sndbuf = atoi(optarg);
			break;
		case 'z':
			zflg = 1;
			break;
		case 'M':
			mss = atoi(optarg);
			break;
		case 'x':
			xflg = 1;
			break;
		case 'k':
			keepflag = 1;
			break;
		case 'P':
			max_pacing_rate = atoi(optarg) ;
			break;
		default:
			exit(1);
		}
	}
	if (sflg) {
		int fdlisten = socket(cfg_family, SOCK_STREAM, 0);

		if (fdlisten == -1) {
			perror("socket");
			exit(1);
		}
		apply_rcvsnd_buf(fdlisten);
		setsockopt(fdlisten, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on));

		setup_sockaddr(cfg_family, host, &listenaddr);

		if (mss &&
		    setsockopt(fdlisten, IPPROTO_TCP, TCP_MAXSEG,
			       &mss, sizeof(mss)) == -1) {
			perror("setsockopt TCP_MAXSEG");
			exit(1);
		}
		if (bind(fdlisten, (const struct sockaddr *)&listenaddr, cfg_alen) == -1) {
			perror("bind");
			exit(1);
		}
		if (listen(fdlisten, 128) == -1) {
			perror("listen");
			exit(1);
		}
		do_accept(fdlisten);
	}
	buffer = mmap(NULL, chunk_size, PROT_READ | PROT_WRITE,
			      MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	if (buffer == (char *)-1) {
		perror("mmap");
		exit(1);
	}

	fd = socket(cfg_family, SOCK_STREAM, 0);
	if (fd == -1) {
		perror("socket");
		exit(1);
	}
	apply_rcvsnd_buf(fd);

	setup_sockaddr(cfg_family, host, &addr);

	if (mss &&
	    setsockopt(fd, IPPROTO_TCP, TCP_MAXSEG, &mss, sizeof(mss)) == -1) {
		perror("setsockopt TCP_MAXSEG");
		exit(1);
	}
	if (connect(fd, (const struct sockaddr *)&addr, cfg_alen) == -1) {
		perror("connect");
		exit(1);
	}
	if (max_pacing_rate &&
	    setsockopt(fd, SOL_SOCKET, SO_MAX_PACING_RATE,
		       &max_pacing_rate, sizeof(max_pacing_rate)) == -1)
		perror("setsockopt SO_MAX_PACING_RATE");

	if (zflg && setsockopt(fd, SOL_SOCKET, SO_ZEROCOPY,
			       &on, sizeof(on)) == -1) {
		perror("setsockopt SO_ZEROCOPY, (-z option disabled)");
		zflg = 0;
	}
	while (total < FILE_SZ) {
		long wr = FILE_SZ - total;

		if (wr > chunk_size)
			wr = chunk_size;
		/* Note : we just want to fill the pipe with 0 bytes */
		wr = send(fd, buffer, wr, zflg ? MSG_ZEROCOPY : 0);
		if (wr <= 0)
			break;
		total += wr;
	}
	close(fd);
	munmap(buffer, chunk_size);
	return 0;
}
