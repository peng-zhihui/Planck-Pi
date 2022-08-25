// SPDX-License-Identifier: GPL-2.0
/*
 * fs/f2fs/hash.c
 *
 * Copyright (c) 2012 Samsung Electronics Co., Ltd.
 *             http://www.samsung.com/
 *
 * Portions of this code from linux/fs/ext3/hash.c
 *
 * Copyright (C) 2002 by Theodore Ts'o
 */
#include <linux/types.h>
#include <linux/fs.h>
#include <linux/f2fs_fs.h>
#include <linux/cryptohash.h>
#include <linux/pagemap.h>
#include <linux/unicode.h>

#include "f2fs.h"

/*
 * Hashing code copied from ext3
 */
#define DELTA 0x9E3779B9

static void TEA_transform(unsigned int buf[4], unsigned int const in[])
{
	__u32 sum = 0;
	__u32 b0 = buf[0], b1 = buf[1];
	__u32 a = in[0], b = in[1], c = in[2], d = in[3];
	int n = 16;

	do {
		sum += DELTA;
		b0 += ((b1 << 4)+a) ^ (b1+sum) ^ ((b1 >> 5)+b);
		b1 += ((b0 << 4)+c) ^ (b0+sum) ^ ((b0 >> 5)+d);
	} while (--n);

	buf[0] += b0;
	buf[1] += b1;
}

static void str2hashbuf(const unsigned char *msg, size_t len,
				unsigned int *buf, int num)
{
	unsigned pad, val;
	int i;

	pad = (__u32)len | ((__u32)len << 8);
	pad |= pad << 16;

	val = pad;
	if (len > num * 4)
		len = num * 4;
	for (i = 0; i < len; i++) {
		if ((i % 4) == 0)
			val = pad;
		val = msg[i] + (val << 8);
		if ((i % 4) == 3) {
			*buf++ = val;
			val = pad;
			num--;
		}
	}
	if (--num >= 0)
		*buf++ = val;
	while (--num >= 0)
		*buf++ = pad;
}

static f2fs_hash_t __f2fs_dentry_hash(const struct qstr *name_info,
				struct fscrypt_name *fname)
{
	__u32 hash;
	f2fs_hash_t f2fs_hash;
	const unsigned char *p;
	__u32 in[8], buf[4];
	const unsigned char *name = name_info->name;
	size_t len = name_info->len;

	/* encrypted bigname case */
	if (fname && !fname->disk_name.name)
		return cpu_to_le32(fname->hash);

	if (is_dot_dotdot(name_info))
		return 0;

	/* Initialize the default seed for the hash checksum functions */
	buf[0] = 0x67452301;
	buf[1] = 0xefcdab89;
	buf[2] = 0x98badcfe;
	buf[3] = 0x10325476;

	p = name;
	while (1) {
		str2hashbuf(p, len, in, 4);
		TEA_transform(buf, in);
		p += 16;
		if (len <= 16)
			break;
		len -= 16;
	}
	hash = buf[0];
	f2fs_hash = cpu_to_le32(hash & ~F2FS_HASH_COL_BIT);
	return f2fs_hash;
}

f2fs_hash_t f2fs_dentry_hash(const struct inode *dir,
		const struct qstr *name_info, struct fscrypt_name *fname)
{
#ifdef CONFIG_UNICODE
	struct f2fs_sb_info *sbi = F2FS_SB(dir->i_sb);
	const struct unicode_map *um = sbi->s_encoding;
	int r, dlen;
	unsigned char *buff;
	struct qstr folded;

	if (!name_info->len || !IS_CASEFOLDED(dir))
		goto opaque_seq;

	buff = f2fs_kzalloc(sbi, sizeof(char) * PATH_MAX, GFP_KERNEL);
	if (!buff)
		return -ENOMEM;

	dlen = utf8_casefold(um, name_info, buff, PATH_MAX);
	if (dlen < 0) {
		kvfree(buff);
		goto opaque_seq;
	}
	folded.name = buff;
	folded.len = dlen;
	r = __f2fs_dentry_hash(&folded, fname);

	kvfree(buff);
	return r;

opaque_seq:
#endif
	return __f2fs_dentry_hash(name_info, fname);
}
