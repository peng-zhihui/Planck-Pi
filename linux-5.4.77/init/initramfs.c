// SPDX-License-Identifier: GPL-2.0
#include <linux/init.h>
#include <linux/fs.h>
#include <linux/slab.h>
#include <linux/types.h>
#include <linux/fcntl.h>
#include <linux/delay.h>
#include <linux/string.h>
#include <linux/dirent.h>
#include <linux/syscalls.h>
#include <linux/utime.h>
#include <linux/file.h>

static ssize_t __init xwrite(int fd, const char *p, size_t count)
{
	ssize_t out = 0;

	/* sys_write only can write MAX_RW_COUNT aka 2G-4K bytes at most */
	while (count) {
		ssize_t rv = ksys_write(fd, p, count);

		if (rv < 0) {
			if (rv == -EINTR || rv == -EAGAIN)
				continue;
			return out ? out : rv;
		} else if (rv == 0)
			break;

		p += rv;
		out += rv;
		count -= rv;
	}

	return out;
}

static __initdata char *message;
static void __init error(char *x)
{
	if (!message)
		message = x;
}

/* link hash */

#define N_ALIGN(len) ((((len) + 1) & ~3) + 2)

static __initdata struct hash {
	int ino, minor, major;
	umode_t mode;
	struct hash *next;
	char name[N_ALIGN(PATH_MAX)];
} *head[32];

static inline int hash(int major, int minor, int ino)
{
	unsigned long tmp = ino + minor + (major << 3);
	tmp += tmp >> 5;
	return tmp & 31;
}

static char __init *find_link(int major, int minor, int ino,
			      umode_t mode, char *name)
{
	struct hash **p, *q;
	for (p = head + hash(major, minor, ino); *p; p = &(*p)->next) {
		if ((*p)->ino != ino)
			continue;
		if ((*p)->minor != minor)
			continue;
		if ((*p)->major != major)
			continue;
		if (((*p)->mode ^ mode) & S_IFMT)
			continue;
		return (*p)->name;
	}
	q = kmalloc(sizeof(struct hash), GFP_KERNEL);
	if (!q)
		panic("can't allocate link hash entry");
	q->major = major;
	q->minor = minor;
	q->ino = ino;
	q->mode = mode;
	strcpy(q->name, name);
	q->next = NULL;
	*p = q;
	return NULL;
}

static void __init free_hash(void)
{
	struct hash **p, *q;
	for (p = head; p < head + 32; p++) {
		while (*p) {
			q = *p;
			*p = q->next;
			kfree(q);
		}
	}
}

static long __init do_utime(char *filename, time64_t mtime)
{
	struct timespec64 t[2];

	t[0].tv_sec = mtime;
	t[0].tv_nsec = 0;
	t[1].tv_sec = mtime;
	t[1].tv_nsec = 0;

	return do_utimes(AT_FDCWD, filename, t, AT_SYMLINK_NOFOLLOW);
}

static __initdata LIST_HEAD(dir_list);
struct dir_entry {
	struct list_head list;
	char *name;
	time64_t mtime;
};

static void __init dir_add(const char *name, time64_t mtime)
{
	struct dir_entry *de = kmalloc(sizeof(struct dir_entry), GFP_KERNEL);
	if (!de)
		panic("can't allocate dir_entry buffer");
	INIT_LIST_HEAD(&de->list);
	de->name = kstrdup(name, GFP_KERNEL);
	de->mtime = mtime;
	list_add(&de->list, &dir_list);
}

static void __init dir_utime(void)
{
	struct dir_entry *de, *tmp;
	list_for_each_entry_safe(de, tmp, &dir_list, list) {
		list_del(&de->list);
		do_utime(de->name, de->mtime);
		kfree(de->name);
		kfree(de);
	}
}

static __initdata time64_t mtime;

/* cpio header parsing */

static __initdata unsigned long ino, major, minor, nlink;
static __initdata umode_t mode;
static __initdata unsigned long body_len, name_len;
static __initdata uid_t uid;
static __initdata gid_t gid;
static __initdata unsigned rdev;

static void __init parse_header(char *s)
{
	unsigned long parsed[12];
	char buf[9];
	int i;

	buf[8] = '\0';
	for (i = 0, s += 6; i < 12; i++, s += 8) {
		memcpy(buf, s, 8);
		parsed[i] = simple_strtoul(buf, NULL, 16);
	}
	ino = parsed[0];
	mode = parsed[1];
	uid = parsed[2];
	gid = parsed[3];
	nlink = parsed[4];
	mtime = parsed[5]; /* breaks in y2106 */
	body_len = parsed[6];
	major = parsed[7];
	minor = parsed[8];
	rdev = new_encode_dev(MKDEV(parsed[9], parsed[10]));
	name_len = parsed[11];
}

/* FSM */

static __initdata enum state {
	Start,
	Collect,
	GotHeader,
	SkipIt,
	GotName,
	CopyFile,
	GotSymlink,
	Reset
} state, next_state;

static __initdata char *victim;
static unsigned long byte_count __initdata;
static __initdata loff_t this_header, next_header;

static inline void __init eat(unsigned n)
{
	victim += n;
	this_header += n;
	byte_count -= n;
}

static __initdata char *vcollected;
static __initdata char *collected;
static long remains __initdata;
static __initdata char *collect;

static void __init read_into(char *buf, unsigned size, enum state next)
{
	if (byte_count >= size) {
		collected = victim;
		eat(size);
		state = next;
	} else {
		collect = collected = buf;
		remains = size;
		next_state = next;
		state = Collect;
	}
}

static __initdata char *header_buf, *symlink_buf, *name_buf;

static int __init do_start(void)
{
	read_into(header_buf, 110, GotHeader);
	return 0;
}

static int __init do_collect(void)
{
	unsigned long n = remains;
	if (byte_count < n)
		n = byte_count;
	memcpy(collect, victim, n);
	eat(n);
	collect += n;
	if ((remains -= n) != 0)
		return 1;
	state = next_state;
	return 0;
}

static int __init do_header(void)
{
	if (memcmp(collected, "070707", 6)==0) {
		error("incorrect cpio method used: use -H newc option");
		return 1;
	}
	if (memcmp(collected, "070701", 6)) {
		error("no cpio magic");
		return 1;
	}
	parse_header(collected);
	next_header = this_header + N_ALIGN(name_len) + body_len;
	next_header = (next_header + 3) & ~3;
	state = SkipIt;
	if (name_len <= 0 || name_len > PATH_MAX)
		return 0;
	if (S_ISLNK(mode)) {
		if (body_len > PATH_MAX)
			return 0;
		collect = collected = symlink_buf;
		remains = N_ALIGN(name_len) + body_len;
		next_state = GotSymlink;
		state = Collect;
		return 0;
	}
	if (S_ISREG(mode) || !body_len)
		read_into(name_buf, N_ALIGN(name_len), GotName);
	return 0;
}

static int __init do_skip(void)
{
	if (this_header + byte_count < next_header) {
		eat(byte_count);
		return 1;
	} else {
		eat(next_header - this_header);
		state = next_state;
		return 0;
	}
}

static int __init do_reset(void)
{
	while (byte_count && *victim == '\0')
		eat(1);
	if (byte_count && (this_header & 3))
		error("broken padding");
	return 1;
}

static void __init clean_path(char *path, umode_t fmode)
{
	struct kstat st;

	if (!vfs_lstat(path, &st) && (st.mode ^ fmode) & S_IFMT) {
		if (S_ISDIR(st.mode))
			ksys_rmdir(path);
		else
			ksys_unlink(path);
	}
}

static int __init maybe_link(void)
{
	if (nlink >= 2) {
		char *old = find_link(major, minor, ino, mode, collected);
		if (old) {
			clean_path(collected, 0);
			return (ksys_link(old, collected) < 0) ? -1 : 1;
		}
	}
	return 0;
}

static __initdata int wfd;

static int __init do_name(void)
{
	state = SkipIt;
	next_state = Reset;
	if (strcmp(collected, "TRAILER!!!") == 0) {
		free_hash();
		return 0;
	}
	clean_path(collected, mode);
	if (S_ISREG(mode)) {
		int ml = maybe_link();
		if (ml >= 0) {
			int openflags = O_WRONLY|O_CREAT;
			if (ml != 1)
				openflags |= O_TRUNC;
			wfd = ksys_open(collected, openflags, mode);

			if (wfd >= 0) {
				ksys_fchown(wfd, uid, gid);
				ksys_fchmod(wfd, mode);
				if (body_len)
					ksys_ftruncate(wfd, body_len);
				vcollected = kstrdup(collected, GFP_KERNEL);
				state = CopyFile;
			}
		}
	} else if (S_ISDIR(mode)) {
		ksys_mkdir(collected, mode);
		ksys_chown(collected, uid, gid);
		ksys_chmod(collected, mode);
		dir_add(collected, mtime);
	} else if (S_ISBLK(mode) || S_ISCHR(mode) ||
		   S_ISFIFO(mode) || S_ISSOCK(mode)) {
		if (maybe_link() == 0) {
			ksys_mknod(collected, mode, rdev);
			ksys_chown(collected, uid, gid);
			ksys_chmod(collected, mode);
			do_utime(collected, mtime);
		}
	}
	return 0;
}

static int __init do_copy(void)
{
	if (byte_count >= body_len) {
		if (xwrite(wfd, victim, body_len) != body_len)
			error("write error");
		ksys_close(wfd);
		do_utime(vcollected, mtime);
		kfree(vcollected);
		eat(body_len);
		state = SkipIt;
		return 0;
	} else {
		if (xwrite(wfd, victim, byte_count) != byte_count)
			error("write error");
		body_len -= byte_count;
		eat(byte_count);
		return 1;
	}
}

static int __init do_symlink(void)
{
	collected[N_ALIGN(name_len) + body_len] = '\0';
	clean_path(collected, 0);
	ksys_symlink(collected + N_ALIGN(name_len), collected);
	ksys_lchown(collected, uid, gid);
	do_utime(collected, mtime);
	state = SkipIt;
	next_state = Reset;
	return 0;
}

static __initdata int (*actions[])(void) = {
	[Start]		= do_start,
	[Collect]	= do_collect,
	[GotHeader]	= do_header,
	[SkipIt]	= do_skip,
	[GotName]	= do_name,
	[CopyFile]	= do_copy,
	[GotSymlink]	= do_symlink,
	[Reset]		= do_reset,
};

static long __init write_buffer(char *buf, unsigned long len)
{
	byte_count = len;
	victim = buf;

	while (!actions[state]())
		;
	return len - byte_count;
}

static long __init flush_buffer(void *bufv, unsigned long len)
{
	char *buf = (char *) bufv;
	long written;
	long origLen = len;
	if (message)
		return -1;
	while ((written = write_buffer(buf, len)) < len && !message) {
		char c = buf[written];
		if (c == '0') {
			buf += written;
			len -= written;
			state = Start;
		} else if (c == 0) {
			buf += written;
			len -= written;
			state = Reset;
		} else
			error("junk within compressed archive");
	}
	return origLen;
}

static unsigned long my_inptr; /* index of next byte to be processed in inbuf */

#include <linux/decompress/generic.h>

static char * __init unpack_to_rootfs(char *buf, unsigned long len)
{
	long written;
	decompress_fn decompress;
	const char *compress_name;
	static __initdata char msg_buf[64];

	header_buf = kmalloc(110, GFP_KERNEL);
	symlink_buf = kmalloc(PATH_MAX + N_ALIGN(PATH_MAX) + 1, GFP_KERNEL);
	name_buf = kmalloc(N_ALIGN(PATH_MAX), GFP_KERNEL);

	if (!header_buf || !symlink_buf || !name_buf)
		panic("can't allocate buffers");

	state = Start;
	this_header = 0;
	message = NULL;
	while (!message && len) {
		loff_t saved_offset = this_header;
		if (*buf == '0' && !(this_header & 3)) {
			state = Start;
			written = write_buffer(buf, len);
			buf += written;
			len -= written;
			continue;
		}
		if (!*buf) {
			buf++;
			len--;
			this_header++;
			continue;
		}
		this_header = 0;
		decompress = decompress_method(buf, len, &compress_name);
		pr_debug("Detected %s compressed data\n", compress_name);
		if (decompress) {
			int res = decompress(buf, len, NULL, flush_buffer, NULL,
				   &my_inptr, error);
			if (res)
				error("decompressor failed");
		} else if (compress_name) {
			if (!message) {
				snprintf(msg_buf, sizeof msg_buf,
					 "compression method %s not configured",
					 compress_name);
				message = msg_buf;
			}
		} else
			error("invalid magic at start of compressed archive");
		if (state != Reset)
			error("junk at the end of compressed archive");
		this_header = saved_offset + my_inptr;
		buf += my_inptr;
		len -= my_inptr;
	}
	dir_utime();
	kfree(name_buf);
	kfree(symlink_buf);
	kfree(header_buf);
	return message;
}

static int __initdata do_retain_initrd;

static int __init retain_initrd_param(char *str)
{
	if (*str)
		return 0;
	do_retain_initrd = 1;
	return 1;
}
__setup("retain_initrd", retain_initrd_param);

#ifdef CONFIG_ARCH_HAS_KEEPINITRD
static int __init keepinitrd_setup(char *__unused)
{
	do_retain_initrd = 1;
	return 1;
}
__setup("keepinitrd", keepinitrd_setup);
#endif

extern char __initramfs_start[];
extern unsigned long __initramfs_size;
#include <linux/initrd.h>
#include <linux/kexec.h>

void __weak free_initrd_mem(unsigned long start, unsigned long end)
{
	free_reserved_area((void *)start, (void *)end, POISON_FREE_INITMEM,
			"initrd");
}

#ifdef CONFIG_KEXEC_CORE
static bool __init kexec_free_initrd(void)
{
	unsigned long crashk_start = (unsigned long)__va(crashk_res.start);
	unsigned long crashk_end   = (unsigned long)__va(crashk_res.end);

	/*
	 * If the initrd region is overlapped with crashkernel reserved region,
	 * free only memory that is not part of crashkernel region.
	 */
	if (initrd_start >= crashk_end || initrd_end <= crashk_start)
		return false;

	/*
	 * Initialize initrd memory region since the kexec boot does not do.
	 */
	memset((void *)initrd_start, 0, initrd_end - initrd_start);
	if (initrd_start < crashk_start)
		free_initrd_mem(initrd_start, crashk_start);
	if (initrd_end > crashk_end)
		free_initrd_mem(crashk_end, initrd_end);
	return true;
}
#else
static inline bool kexec_free_initrd(void)
{
	return false;
}
#endif /* CONFIG_KEXEC_CORE */

#ifdef CONFIG_BLK_DEV_RAM
#define BUF_SIZE 1024
static void __init clean_rootfs(void)
{
	int fd;
	void *buf;
	struct linux_dirent64 *dirp;
	int num;

	fd = ksys_open("/", O_RDONLY, 0);
	WARN_ON(fd < 0);
	if (fd < 0)
		return;
	buf = kzalloc(BUF_SIZE, GFP_KERNEL);
	WARN_ON(!buf);
	if (!buf) {
		ksys_close(fd);
		return;
	}

	dirp = buf;
	num = ksys_getdents64(fd, dirp, BUF_SIZE);
	while (num > 0) {
		while (num > 0) {
			struct kstat st;
			int ret;

			ret = vfs_lstat(dirp->d_name, &st);
			WARN_ON_ONCE(ret);
			if (!ret) {
				if (S_ISDIR(st.mode))
					ksys_rmdir(dirp->d_name);
				else
					ksys_unlink(dirp->d_name);
			}

			num -= dirp->d_reclen;
			dirp = (void *)dirp + dirp->d_reclen;
		}
		dirp = buf;
		memset(buf, 0, BUF_SIZE);
		num = ksys_getdents64(fd, dirp, BUF_SIZE);
	}

	ksys_close(fd);
	kfree(buf);
}
#else
static inline void clean_rootfs(void)
{
}
#endif /* CONFIG_BLK_DEV_RAM */

#ifdef CONFIG_BLK_DEV_RAM
static void __init populate_initrd_image(char *err)
{
	ssize_t written;
	int fd;

	unpack_to_rootfs(__initramfs_start, __initramfs_size);

	printk(KERN_INFO "rootfs image is not initramfs (%s); looks like an initrd\n",
			err);
	fd = ksys_open("/initrd.image", O_WRONLY | O_CREAT, 0700);
	if (fd < 0)
		return;

	written = xwrite(fd, (char *)initrd_start, initrd_end - initrd_start);
	if (written != initrd_end - initrd_start)
		pr_err("/initrd.image: incomplete write (%zd != %ld)\n",
		       written, initrd_end - initrd_start);
	ksys_close(fd);
}
#else
static void __init populate_initrd_image(char *err)
{
	printk(KERN_EMERG "Initramfs unpacking failed: %s\n", err);
}
#endif /* CONFIG_BLK_DEV_RAM */

static int __init populate_rootfs(void)
{
	/* Load the built in initramfs */
	char *err = unpack_to_rootfs(__initramfs_start, __initramfs_size);
	if (err)
		panic("%s", err); /* Failed to decompress INTERNAL initramfs */

	if (!initrd_start || IS_ENABLED(CONFIG_INITRAMFS_FORCE))
		goto done;

	if (IS_ENABLED(CONFIG_BLK_DEV_RAM))
		printk(KERN_INFO "Trying to unpack rootfs image as initramfs...\n");
	else
		printk(KERN_INFO "Unpacking initramfs...\n");

	err = unpack_to_rootfs((char *)initrd_start, initrd_end - initrd_start);
	if (err) {
		clean_rootfs();
		populate_initrd_image(err);
	}

done:
	/*
	 * If the initrd region is overlapped with crashkernel reserved region,
	 * free only memory that is not part of crashkernel region.
	 */
	if (!do_retain_initrd && initrd_start && !kexec_free_initrd())
		free_initrd_mem(initrd_start, initrd_end);
	initrd_start = 0;
	initrd_end = 0;

	flush_delayed_fput();
	return 0;
}
rootfs_initcall(populate_rootfs);
