// SPDX-License-Identifier: GPL-2.0
/*
 * Cryptographic API.
 *
 * s390 implementation of the AES Cipher Algorithm with protected keys.
 *
 * s390 Version:
 *   Copyright IBM Corp. 2017,2019
 *   Author(s): Martin Schwidefsky <schwidefsky@de.ibm.com>
 *		Harald Freudenberger <freude@de.ibm.com>
 */

#define KMSG_COMPONENT "paes_s390"
#define pr_fmt(fmt) KMSG_COMPONENT ": " fmt

#include <crypto/aes.h>
#include <crypto/algapi.h>
#include <linux/bug.h>
#include <linux/err.h>
#include <linux/module.h>
#include <linux/cpufeature.h>
#include <linux/init.h>
#include <linux/spinlock.h>
#include <crypto/xts.h>
#include <asm/cpacf.h>
#include <asm/pkey.h>

/*
 * Key blobs smaller/bigger than these defines are rejected
 * by the common code even before the individual setkey function
 * is called. As paes can handle different kinds of key blobs
 * and padding is also possible, the limits need to be generous.
 */
#define PAES_MIN_KEYSIZE 64
#define PAES_MAX_KEYSIZE 256

static u8 *ctrblk;
static DEFINE_SPINLOCK(ctrblk_lock);

static cpacf_mask_t km_functions, kmc_functions, kmctr_functions;

struct key_blob {
	/*
	 * Small keys will be stored in the keybuf. Larger keys are
	 * stored in extra allocated memory. In both cases does
	 * key point to the memory where the key is stored.
	 * The code distinguishes by checking keylen against
	 * sizeof(keybuf). See the two following helper functions.
	 */
	u8 *key;
	u8 keybuf[128];
	unsigned int keylen;
};

static inline int _copy_key_to_kb(struct key_blob *kb,
				  const u8 *key,
				  unsigned int keylen)
{
	if (keylen <= sizeof(kb->keybuf))
		kb->key = kb->keybuf;
	else {
		kb->key = kmalloc(keylen, GFP_KERNEL);
		if (!kb->key)
			return -ENOMEM;
	}
	memcpy(kb->key, key, keylen);
	kb->keylen = keylen;

	return 0;
}

static inline void _free_kb_keybuf(struct key_blob *kb)
{
	if (kb->key && kb->key != kb->keybuf
	    && kb->keylen > sizeof(kb->keybuf)) {
		kfree(kb->key);
		kb->key = NULL;
	}
}

struct s390_paes_ctx {
	struct key_blob kb;
	struct pkey_protkey pk;
	unsigned long fc;
};

struct s390_pxts_ctx {
	struct key_blob kb[2];
	struct pkey_protkey pk[2];
	unsigned long fc;
};

static inline int __paes_convert_key(struct key_blob *kb,
				     struct pkey_protkey *pk)
{
	int i, ret;

	/* try three times in case of failure */
	for (i = 0; i < 3; i++) {
		ret = pkey_keyblob2pkey(kb->key, kb->keylen, pk);
		if (ret == 0)
			break;
	}

	return ret;
}

static int __paes_set_key(struct s390_paes_ctx *ctx)
{
	unsigned long fc;

	if (__paes_convert_key(&ctx->kb, &ctx->pk))
		return -EINVAL;

	/* Pick the correct function code based on the protected key type */
	fc = (ctx->pk.type == PKEY_KEYTYPE_AES_128) ? CPACF_KM_PAES_128 :
		(ctx->pk.type == PKEY_KEYTYPE_AES_192) ? CPACF_KM_PAES_192 :
		(ctx->pk.type == PKEY_KEYTYPE_AES_256) ? CPACF_KM_PAES_256 : 0;

	/* Check if the function code is available */
	ctx->fc = (fc && cpacf_test_func(&km_functions, fc)) ? fc : 0;

	return ctx->fc ? 0 : -EINVAL;
}

static int ecb_paes_init(struct crypto_tfm *tfm)
{
	struct s390_paes_ctx *ctx = crypto_tfm_ctx(tfm);

	ctx->kb.key = NULL;

	return 0;
}

static void ecb_paes_exit(struct crypto_tfm *tfm)
{
	struct s390_paes_ctx *ctx = crypto_tfm_ctx(tfm);

	_free_kb_keybuf(&ctx->kb);
}

static int ecb_paes_set_key(struct crypto_tfm *tfm, const u8 *in_key,
			    unsigned int key_len)
{
	int rc;
	struct s390_paes_ctx *ctx = crypto_tfm_ctx(tfm);

	_free_kb_keybuf(&ctx->kb);
	rc = _copy_key_to_kb(&ctx->kb, in_key, key_len);
	if (rc)
		return rc;

	if (__paes_set_key(ctx)) {
		tfm->crt_flags |= CRYPTO_TFM_RES_BAD_KEY_LEN;
		return -EINVAL;
	}
	return 0;
}

static int ecb_paes_crypt(struct blkcipher_desc *desc,
			  unsigned long modifier,
			  struct blkcipher_walk *walk)
{
	struct s390_paes_ctx *ctx = crypto_blkcipher_ctx(desc->tfm);
	unsigned int nbytes, n, k;
	int ret;

	ret = blkcipher_walk_virt(desc, walk);
	while ((nbytes = walk->nbytes) >= AES_BLOCK_SIZE) {
		/* only use complete blocks */
		n = nbytes & ~(AES_BLOCK_SIZE - 1);
		k = cpacf_km(ctx->fc | modifier, ctx->pk.protkey,
			     walk->dst.virt.addr, walk->src.virt.addr, n);
		if (k)
			ret = blkcipher_walk_done(desc, walk, nbytes - k);
		if (k < n) {
			if (__paes_set_key(ctx) != 0)
				return blkcipher_walk_done(desc, walk, -EIO);
		}
	}
	return ret;
}

static int ecb_paes_encrypt(struct blkcipher_desc *desc,
			    struct scatterlist *dst, struct scatterlist *src,
			    unsigned int nbytes)
{
	struct blkcipher_walk walk;

	blkcipher_walk_init(&walk, dst, src, nbytes);
	return ecb_paes_crypt(desc, CPACF_ENCRYPT, &walk);
}

static int ecb_paes_decrypt(struct blkcipher_desc *desc,
			    struct scatterlist *dst, struct scatterlist *src,
			    unsigned int nbytes)
{
	struct blkcipher_walk walk;

	blkcipher_walk_init(&walk, dst, src, nbytes);
	return ecb_paes_crypt(desc, CPACF_DECRYPT, &walk);
}

static struct crypto_alg ecb_paes_alg = {
	.cra_name		=	"ecb(paes)",
	.cra_driver_name	=	"ecb-paes-s390",
	.cra_priority		=	401,	/* combo: aes + ecb + 1 */
	.cra_flags		=	CRYPTO_ALG_TYPE_BLKCIPHER,
	.cra_blocksize		=	AES_BLOCK_SIZE,
	.cra_ctxsize		=	sizeof(struct s390_paes_ctx),
	.cra_type		=	&crypto_blkcipher_type,
	.cra_module		=	THIS_MODULE,
	.cra_list		=	LIST_HEAD_INIT(ecb_paes_alg.cra_list),
	.cra_init		=	ecb_paes_init,
	.cra_exit		=	ecb_paes_exit,
	.cra_u			=	{
		.blkcipher = {
			.min_keysize		=	PAES_MIN_KEYSIZE,
			.max_keysize		=	PAES_MAX_KEYSIZE,
			.setkey			=	ecb_paes_set_key,
			.encrypt		=	ecb_paes_encrypt,
			.decrypt		=	ecb_paes_decrypt,
		}
	}
};

static int cbc_paes_init(struct crypto_tfm *tfm)
{
	struct s390_paes_ctx *ctx = crypto_tfm_ctx(tfm);

	ctx->kb.key = NULL;

	return 0;
}

static void cbc_paes_exit(struct crypto_tfm *tfm)
{
	struct s390_paes_ctx *ctx = crypto_tfm_ctx(tfm);

	_free_kb_keybuf(&ctx->kb);
}

static int __cbc_paes_set_key(struct s390_paes_ctx *ctx)
{
	unsigned long fc;

	if (__paes_convert_key(&ctx->kb, &ctx->pk))
		return -EINVAL;

	/* Pick the correct function code based on the protected key type */
	fc = (ctx->pk.type == PKEY_KEYTYPE_AES_128) ? CPACF_KMC_PAES_128 :
		(ctx->pk.type == PKEY_KEYTYPE_AES_192) ? CPACF_KMC_PAES_192 :
		(ctx->pk.type == PKEY_KEYTYPE_AES_256) ? CPACF_KMC_PAES_256 : 0;

	/* Check if the function code is available */
	ctx->fc = (fc && cpacf_test_func(&kmc_functions, fc)) ? fc : 0;

	return ctx->fc ? 0 : -EINVAL;
}

static int cbc_paes_set_key(struct crypto_tfm *tfm, const u8 *in_key,
			    unsigned int key_len)
{
	int rc;
	struct s390_paes_ctx *ctx = crypto_tfm_ctx(tfm);

	_free_kb_keybuf(&ctx->kb);
	rc = _copy_key_to_kb(&ctx->kb, in_key, key_len);
	if (rc)
		return rc;

	if (__cbc_paes_set_key(ctx)) {
		tfm->crt_flags |= CRYPTO_TFM_RES_BAD_KEY_LEN;
		return -EINVAL;
	}
	return 0;
}

static int cbc_paes_crypt(struct blkcipher_desc *desc, unsigned long modifier,
			  struct blkcipher_walk *walk)
{
	struct s390_paes_ctx *ctx = crypto_blkcipher_ctx(desc->tfm);
	unsigned int nbytes, n, k;
	int ret;
	struct {
		u8 iv[AES_BLOCK_SIZE];
		u8 key[MAXPROTKEYSIZE];
	} param;

	ret = blkcipher_walk_virt(desc, walk);
	memcpy(param.iv, walk->iv, AES_BLOCK_SIZE);
	memcpy(param.key, ctx->pk.protkey, MAXPROTKEYSIZE);
	while ((nbytes = walk->nbytes) >= AES_BLOCK_SIZE) {
		/* only use complete blocks */
		n = nbytes & ~(AES_BLOCK_SIZE - 1);
		k = cpacf_kmc(ctx->fc | modifier, &param,
			      walk->dst.virt.addr, walk->src.virt.addr, n);
		if (k)
			ret = blkcipher_walk_done(desc, walk, nbytes - k);
		if (k < n) {
			if (__cbc_paes_set_key(ctx) != 0)
				return blkcipher_walk_done(desc, walk, -EIO);
			memcpy(param.key, ctx->pk.protkey, MAXPROTKEYSIZE);
		}
	}
	memcpy(walk->iv, param.iv, AES_BLOCK_SIZE);
	return ret;
}

static int cbc_paes_encrypt(struct blkcipher_desc *desc,
			    struct scatterlist *dst, struct scatterlist *src,
			    unsigned int nbytes)
{
	struct blkcipher_walk walk;

	blkcipher_walk_init(&walk, dst, src, nbytes);
	return cbc_paes_crypt(desc, 0, &walk);
}

static int cbc_paes_decrypt(struct blkcipher_desc *desc,
			    struct scatterlist *dst, struct scatterlist *src,
			    unsigned int nbytes)
{
	struct blkcipher_walk walk;

	blkcipher_walk_init(&walk, dst, src, nbytes);
	return cbc_paes_crypt(desc, CPACF_DECRYPT, &walk);
}

static struct crypto_alg cbc_paes_alg = {
	.cra_name		=	"cbc(paes)",
	.cra_driver_name	=	"cbc-paes-s390",
	.cra_priority		=	402,	/* ecb-paes-s390 + 1 */
	.cra_flags		=	CRYPTO_ALG_TYPE_BLKCIPHER,
	.cra_blocksize		=	AES_BLOCK_SIZE,
	.cra_ctxsize		=	sizeof(struct s390_paes_ctx),
	.cra_type		=	&crypto_blkcipher_type,
	.cra_module		=	THIS_MODULE,
	.cra_list		=	LIST_HEAD_INIT(cbc_paes_alg.cra_list),
	.cra_init		=	cbc_paes_init,
	.cra_exit		=	cbc_paes_exit,
	.cra_u			=	{
		.blkcipher = {
			.min_keysize		=	PAES_MIN_KEYSIZE,
			.max_keysize		=	PAES_MAX_KEYSIZE,
			.ivsize			=	AES_BLOCK_SIZE,
			.setkey			=	cbc_paes_set_key,
			.encrypt		=	cbc_paes_encrypt,
			.decrypt		=	cbc_paes_decrypt,
		}
	}
};

static int xts_paes_init(struct crypto_tfm *tfm)
{
	struct s390_pxts_ctx *ctx = crypto_tfm_ctx(tfm);

	ctx->kb[0].key = NULL;
	ctx->kb[1].key = NULL;

	return 0;
}

static void xts_paes_exit(struct crypto_tfm *tfm)
{
	struct s390_pxts_ctx *ctx = crypto_tfm_ctx(tfm);

	_free_kb_keybuf(&ctx->kb[0]);
	_free_kb_keybuf(&ctx->kb[1]);
}

static int __xts_paes_set_key(struct s390_pxts_ctx *ctx)
{
	unsigned long fc;

	if (__paes_convert_key(&ctx->kb[0], &ctx->pk[0]) ||
	    __paes_convert_key(&ctx->kb[1], &ctx->pk[1]))
		return -EINVAL;

	if (ctx->pk[0].type != ctx->pk[1].type)
		return -EINVAL;

	/* Pick the correct function code based on the protected key type */
	fc = (ctx->pk[0].type == PKEY_KEYTYPE_AES_128) ? CPACF_KM_PXTS_128 :
		(ctx->pk[0].type == PKEY_KEYTYPE_AES_256) ?
		CPACF_KM_PXTS_256 : 0;

	/* Check if the function code is available */
	ctx->fc = (fc && cpacf_test_func(&km_functions, fc)) ? fc : 0;

	return ctx->fc ? 0 : -EINVAL;
}

static int xts_paes_set_key(struct crypto_tfm *tfm, const u8 *in_key,
			    unsigned int xts_key_len)
{
	int rc;
	struct s390_pxts_ctx *ctx = crypto_tfm_ctx(tfm);
	u8 ckey[2 * AES_MAX_KEY_SIZE];
	unsigned int ckey_len, key_len;

	if (xts_key_len % 2)
		return -EINVAL;

	key_len = xts_key_len / 2;

	_free_kb_keybuf(&ctx->kb[0]);
	_free_kb_keybuf(&ctx->kb[1]);
	rc = _copy_key_to_kb(&ctx->kb[0], in_key, key_len);
	if (rc)
		return rc;
	rc = _copy_key_to_kb(&ctx->kb[1], in_key + key_len, key_len);
	if (rc)
		return rc;

	if (__xts_paes_set_key(ctx)) {
		tfm->crt_flags |= CRYPTO_TFM_RES_BAD_KEY_LEN;
		return -EINVAL;
	}

	/*
	 * xts_check_key verifies the key length is not odd and makes
	 * sure that the two keys are not the same. This can be done
	 * on the two protected keys as well
	 */
	ckey_len = (ctx->pk[0].type == PKEY_KEYTYPE_AES_128) ?
		AES_KEYSIZE_128 : AES_KEYSIZE_256;
	memcpy(ckey, ctx->pk[0].protkey, ckey_len);
	memcpy(ckey + ckey_len, ctx->pk[1].protkey, ckey_len);
	return xts_check_key(tfm, ckey, 2*ckey_len);
}

static int xts_paes_crypt(struct blkcipher_desc *desc, unsigned long modifier,
			  struct blkcipher_walk *walk)
{
	struct s390_pxts_ctx *ctx = crypto_blkcipher_ctx(desc->tfm);
	unsigned int keylen, offset, nbytes, n, k;
	int ret;
	struct {
		u8 key[MAXPROTKEYSIZE];	/* key + verification pattern */
		u8 tweak[16];
		u8 block[16];
		u8 bit[16];
		u8 xts[16];
	} pcc_param;
	struct {
		u8 key[MAXPROTKEYSIZE];	/* key + verification pattern */
		u8 init[16];
	} xts_param;

	ret = blkcipher_walk_virt(desc, walk);
	keylen = (ctx->pk[0].type == PKEY_KEYTYPE_AES_128) ? 48 : 64;
	offset = (ctx->pk[0].type == PKEY_KEYTYPE_AES_128) ? 16 : 0;
retry:
	memset(&pcc_param, 0, sizeof(pcc_param));
	memcpy(pcc_param.tweak, walk->iv, sizeof(pcc_param.tweak));
	memcpy(pcc_param.key + offset, ctx->pk[1].protkey, keylen);
	cpacf_pcc(ctx->fc, pcc_param.key + offset);

	memcpy(xts_param.key + offset, ctx->pk[0].protkey, keylen);
	memcpy(xts_param.init, pcc_param.xts, 16);

	while ((nbytes = walk->nbytes) >= AES_BLOCK_SIZE) {
		/* only use complete blocks */
		n = nbytes & ~(AES_BLOCK_SIZE - 1);
		k = cpacf_km(ctx->fc | modifier, xts_param.key + offset,
			     walk->dst.virt.addr, walk->src.virt.addr, n);
		if (k)
			ret = blkcipher_walk_done(desc, walk, nbytes - k);
		if (k < n) {
			if (__xts_paes_set_key(ctx) != 0)
				return blkcipher_walk_done(desc, walk, -EIO);
			goto retry;
		}
	}
	return ret;
}

static int xts_paes_encrypt(struct blkcipher_desc *desc,
			    struct scatterlist *dst, struct scatterlist *src,
			    unsigned int nbytes)
{
	struct blkcipher_walk walk;

	blkcipher_walk_init(&walk, dst, src, nbytes);
	return xts_paes_crypt(desc, 0, &walk);
}

static int xts_paes_decrypt(struct blkcipher_desc *desc,
			    struct scatterlist *dst, struct scatterlist *src,
			    unsigned int nbytes)
{
	struct blkcipher_walk walk;

	blkcipher_walk_init(&walk, dst, src, nbytes);
	return xts_paes_crypt(desc, CPACF_DECRYPT, &walk);
}

static struct crypto_alg xts_paes_alg = {
	.cra_name		=	"xts(paes)",
	.cra_driver_name	=	"xts-paes-s390",
	.cra_priority		=	402,	/* ecb-paes-s390 + 1 */
	.cra_flags		=	CRYPTO_ALG_TYPE_BLKCIPHER,
	.cra_blocksize		=	AES_BLOCK_SIZE,
	.cra_ctxsize		=	sizeof(struct s390_pxts_ctx),
	.cra_type		=	&crypto_blkcipher_type,
	.cra_module		=	THIS_MODULE,
	.cra_list		=	LIST_HEAD_INIT(xts_paes_alg.cra_list),
	.cra_init		=	xts_paes_init,
	.cra_exit		=	xts_paes_exit,
	.cra_u			=	{
		.blkcipher = {
			.min_keysize		=	2 * PAES_MIN_KEYSIZE,
			.max_keysize		=	2 * PAES_MAX_KEYSIZE,
			.ivsize			=	AES_BLOCK_SIZE,
			.setkey			=	xts_paes_set_key,
			.encrypt		=	xts_paes_encrypt,
			.decrypt		=	xts_paes_decrypt,
		}
	}
};

static int ctr_paes_init(struct crypto_tfm *tfm)
{
	struct s390_paes_ctx *ctx = crypto_tfm_ctx(tfm);

	ctx->kb.key = NULL;

	return 0;
}

static void ctr_paes_exit(struct crypto_tfm *tfm)
{
	struct s390_paes_ctx *ctx = crypto_tfm_ctx(tfm);

	_free_kb_keybuf(&ctx->kb);
}

static int __ctr_paes_set_key(struct s390_paes_ctx *ctx)
{
	unsigned long fc;

	if (__paes_convert_key(&ctx->kb, &ctx->pk))
		return -EINVAL;

	/* Pick the correct function code based on the protected key type */
	fc = (ctx->pk.type == PKEY_KEYTYPE_AES_128) ? CPACF_KMCTR_PAES_128 :
		(ctx->pk.type == PKEY_KEYTYPE_AES_192) ? CPACF_KMCTR_PAES_192 :
		(ctx->pk.type == PKEY_KEYTYPE_AES_256) ?
		CPACF_KMCTR_PAES_256 : 0;

	/* Check if the function code is available */
	ctx->fc = (fc && cpacf_test_func(&kmctr_functions, fc)) ? fc : 0;

	return ctx->fc ? 0 : -EINVAL;
}

static int ctr_paes_set_key(struct crypto_tfm *tfm, const u8 *in_key,
			    unsigned int key_len)
{
	int rc;
	struct s390_paes_ctx *ctx = crypto_tfm_ctx(tfm);

	_free_kb_keybuf(&ctx->kb);
	rc = _copy_key_to_kb(&ctx->kb, in_key, key_len);
	if (rc)
		return rc;

	if (__ctr_paes_set_key(ctx)) {
		tfm->crt_flags |= CRYPTO_TFM_RES_BAD_KEY_LEN;
		return -EINVAL;
	}
	return 0;
}

static unsigned int __ctrblk_init(u8 *ctrptr, u8 *iv, unsigned int nbytes)
{
	unsigned int i, n;

	/* only use complete blocks, max. PAGE_SIZE */
	memcpy(ctrptr, iv, AES_BLOCK_SIZE);
	n = (nbytes > PAGE_SIZE) ? PAGE_SIZE : nbytes & ~(AES_BLOCK_SIZE - 1);
	for (i = (n / AES_BLOCK_SIZE) - 1; i > 0; i--) {
		memcpy(ctrptr + AES_BLOCK_SIZE, ctrptr, AES_BLOCK_SIZE);
		crypto_inc(ctrptr + AES_BLOCK_SIZE, AES_BLOCK_SIZE);
		ctrptr += AES_BLOCK_SIZE;
	}
	return n;
}

static int ctr_paes_crypt(struct blkcipher_desc *desc, unsigned long modifier,
			  struct blkcipher_walk *walk)
{
	struct s390_paes_ctx *ctx = crypto_blkcipher_ctx(desc->tfm);
	u8 buf[AES_BLOCK_SIZE], *ctrptr;
	unsigned int nbytes, n, k;
	int ret, locked;

	locked = spin_trylock(&ctrblk_lock);

	ret = blkcipher_walk_virt_block(desc, walk, AES_BLOCK_SIZE);
	while ((nbytes = walk->nbytes) >= AES_BLOCK_SIZE) {
		n = AES_BLOCK_SIZE;
		if (nbytes >= 2*AES_BLOCK_SIZE && locked)
			n = __ctrblk_init(ctrblk, walk->iv, nbytes);
		ctrptr = (n > AES_BLOCK_SIZE) ? ctrblk : walk->iv;
		k = cpacf_kmctr(ctx->fc | modifier, ctx->pk.protkey,
				walk->dst.virt.addr, walk->src.virt.addr,
				n, ctrptr);
		if (k) {
			if (ctrptr == ctrblk)
				memcpy(walk->iv, ctrptr + k - AES_BLOCK_SIZE,
				       AES_BLOCK_SIZE);
			crypto_inc(walk->iv, AES_BLOCK_SIZE);
			ret = blkcipher_walk_done(desc, walk, nbytes - n);
		}
		if (k < n) {
			if (__ctr_paes_set_key(ctx) != 0) {
				if (locked)
					spin_unlock(&ctrblk_lock);
				return blkcipher_walk_done(desc, walk, -EIO);
			}
		}
	}
	if (locked)
		spin_unlock(&ctrblk_lock);
	/*
	 * final block may be < AES_BLOCK_SIZE, copy only nbytes
	 */
	if (nbytes) {
		while (1) {
			if (cpacf_kmctr(ctx->fc | modifier,
					ctx->pk.protkey, buf,
					walk->src.virt.addr, AES_BLOCK_SIZE,
					walk->iv) == AES_BLOCK_SIZE)
				break;
			if (__ctr_paes_set_key(ctx) != 0)
				return blkcipher_walk_done(desc, walk, -EIO);
		}
		memcpy(walk->dst.virt.addr, buf, nbytes);
		crypto_inc(walk->iv, AES_BLOCK_SIZE);
		ret = blkcipher_walk_done(desc, walk, 0);
	}

	return ret;
}

static int ctr_paes_encrypt(struct blkcipher_desc *desc,
			    struct scatterlist *dst, struct scatterlist *src,
			    unsigned int nbytes)
{
	struct blkcipher_walk walk;

	blkcipher_walk_init(&walk, dst, src, nbytes);
	return ctr_paes_crypt(desc, 0, &walk);
}

static int ctr_paes_decrypt(struct blkcipher_desc *desc,
			    struct scatterlist *dst, struct scatterlist *src,
			    unsigned int nbytes)
{
	struct blkcipher_walk walk;

	blkcipher_walk_init(&walk, dst, src, nbytes);
	return ctr_paes_crypt(desc, CPACF_DECRYPT, &walk);
}

static struct crypto_alg ctr_paes_alg = {
	.cra_name		=	"ctr(paes)",
	.cra_driver_name	=	"ctr-paes-s390",
	.cra_priority		=	402,	/* ecb-paes-s390 + 1 */
	.cra_flags		=	CRYPTO_ALG_TYPE_BLKCIPHER,
	.cra_blocksize		=	1,
	.cra_ctxsize		=	sizeof(struct s390_paes_ctx),
	.cra_type		=	&crypto_blkcipher_type,
	.cra_module		=	THIS_MODULE,
	.cra_list		=	LIST_HEAD_INIT(ctr_paes_alg.cra_list),
	.cra_init		=	ctr_paes_init,
	.cra_exit		=	ctr_paes_exit,
	.cra_u			=	{
		.blkcipher = {
			.min_keysize		=	PAES_MIN_KEYSIZE,
			.max_keysize		=	PAES_MAX_KEYSIZE,
			.ivsize			=	AES_BLOCK_SIZE,
			.setkey			=	ctr_paes_set_key,
			.encrypt		=	ctr_paes_encrypt,
			.decrypt		=	ctr_paes_decrypt,
		}
	}
};

static inline void __crypto_unregister_alg(struct crypto_alg *alg)
{
	if (!list_empty(&alg->cra_list))
		crypto_unregister_alg(alg);
}

static void paes_s390_fini(void)
{
	if (ctrblk)
		free_page((unsigned long) ctrblk);
	__crypto_unregister_alg(&ctr_paes_alg);
	__crypto_unregister_alg(&xts_paes_alg);
	__crypto_unregister_alg(&cbc_paes_alg);
	__crypto_unregister_alg(&ecb_paes_alg);
}

static int __init paes_s390_init(void)
{
	int ret;

	/* Query available functions for KM, KMC and KMCTR */
	cpacf_query(CPACF_KM, &km_functions);
	cpacf_query(CPACF_KMC, &kmc_functions);
	cpacf_query(CPACF_KMCTR, &kmctr_functions);

	if (cpacf_test_func(&km_functions, CPACF_KM_PAES_128) ||
	    cpacf_test_func(&km_functions, CPACF_KM_PAES_192) ||
	    cpacf_test_func(&km_functions, CPACF_KM_PAES_256)) {
		ret = crypto_register_alg(&ecb_paes_alg);
		if (ret)
			goto out_err;
	}

	if (cpacf_test_func(&kmc_functions, CPACF_KMC_PAES_128) ||
	    cpacf_test_func(&kmc_functions, CPACF_KMC_PAES_192) ||
	    cpacf_test_func(&kmc_functions, CPACF_KMC_PAES_256)) {
		ret = crypto_register_alg(&cbc_paes_alg);
		if (ret)
			goto out_err;
	}

	if (cpacf_test_func(&km_functions, CPACF_KM_PXTS_128) ||
	    cpacf_test_func(&km_functions, CPACF_KM_PXTS_256)) {
		ret = crypto_register_alg(&xts_paes_alg);
		if (ret)
			goto out_err;
	}

	if (cpacf_test_func(&kmctr_functions, CPACF_KMCTR_PAES_128) ||
	    cpacf_test_func(&kmctr_functions, CPACF_KMCTR_PAES_192) ||
	    cpacf_test_func(&kmctr_functions, CPACF_KMCTR_PAES_256)) {
		ret = crypto_register_alg(&ctr_paes_alg);
		if (ret)
			goto out_err;
		ctrblk = (u8 *) __get_free_page(GFP_KERNEL);
		if (!ctrblk) {
			ret = -ENOMEM;
			goto out_err;
		}
	}

	return 0;
out_err:
	paes_s390_fini();
	return ret;
}

module_init(paes_s390_init);
module_exit(paes_s390_fini);

MODULE_ALIAS_CRYPTO("paes");

MODULE_DESCRIPTION("Rijndael (AES) Cipher Algorithm with protected keys");
MODULE_LICENSE("GPL");
