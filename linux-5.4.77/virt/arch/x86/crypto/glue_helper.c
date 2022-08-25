// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Shared glue code for 128bit block ciphers
 *
 * Copyright © 2012-2013 Jussi Kivilinna <jussi.kivilinna@iki.fi>
 *
 * CBC & ECB parts based on code (crypto/cbc.c,ecb.c) by:
 *   Copyright (c) 2006 Herbert Xu <herbert@gondor.apana.org.au>
 * CTR part based on code (crypto/ctr.c) by:
 *   (C) Copyright IBM Corp. 2007 - Joy Latten <latten@us.ibm.com>
 */

#include <linux/module.h>
#include <crypto/b128ops.h>
#include <crypto/gf128mul.h>
#include <crypto/internal/skcipher.h>
#include <crypto/scatterwalk.h>
#include <crypto/xts.h>
#include <asm/crypto/glue_helper.h>

int glue_ecb_req_128bit(const struct common_glue_ctx *gctx,
			struct skcipher_request *req)
{
	void *ctx = crypto_skcipher_ctx(crypto_skcipher_reqtfm(req));
	const unsigned int bsize = 128 / 8;
	struct skcipher_walk walk;
	bool fpu_enabled = false;
	unsigned int nbytes;
	int err;

	err = skcipher_walk_virt(&walk, req, false);

	while ((nbytes = walk.nbytes)) {
		const u8 *src = walk.src.virt.addr;
		u8 *dst = walk.dst.virt.addr;
		unsigned int func_bytes;
		unsigned int i;

		fpu_enabled = glue_fpu_begin(bsize, gctx->fpu_blocks_limit,
					     &walk, fpu_enabled, nbytes);
		for (i = 0; i < gctx->num_funcs; i++) {
			func_bytes = bsize * gctx->funcs[i].num_blocks;

			if (nbytes < func_bytes)
				continue;

			/* Process multi-block batch */
			do {
				gctx->funcs[i].fn_u.ecb(ctx, dst, src);
				src += func_bytes;
				dst += func_bytes;
				nbytes -= func_bytes;
			} while (nbytes >= func_bytes);

			if (nbytes < bsize)
				break;
		}
		err = skcipher_walk_done(&walk, nbytes);
	}

	glue_fpu_end(fpu_enabled);
	return err;
}
EXPORT_SYMBOL_GPL(glue_ecb_req_128bit);

int glue_cbc_encrypt_req_128bit(const common_glue_func_t fn,
				struct skcipher_request *req)
{
	void *ctx = crypto_skcipher_ctx(crypto_skcipher_reqtfm(req));
	const unsigned int bsize = 128 / 8;
	struct skcipher_walk walk;
	unsigned int nbytes;
	int err;

	err = skcipher_walk_virt(&walk, req, false);

	while ((nbytes = walk.nbytes)) {
		const u128 *src = (u128 *)walk.src.virt.addr;
		u128 *dst = (u128 *)walk.dst.virt.addr;
		u128 *iv = (u128 *)walk.iv;

		do {
			u128_xor(dst, src, iv);
			fn(ctx, (u8 *)dst, (u8 *)dst);
			iv = dst;
			src++;
			dst++;
			nbytes -= bsize;
		} while (nbytes >= bsize);

		*(u128 *)walk.iv = *iv;
		err = skcipher_walk_done(&walk, nbytes);
	}
	return err;
}
EXPORT_SYMBOL_GPL(glue_cbc_encrypt_req_128bit);

int glue_cbc_decrypt_req_128bit(const struct common_glue_ctx *gctx,
				struct skcipher_request *req)
{
	void *ctx = crypto_skcipher_ctx(crypto_skcipher_reqtfm(req));
	const unsigned int bsize = 128 / 8;
	struct skcipher_walk walk;
	bool fpu_enabled = false;
	unsigned int nbytes;
	int err;

	err = skcipher_walk_virt(&walk, req, false);

	while ((nbytes = walk.nbytes)) {
		const u128 *src = walk.src.virt.addr;
		u128 *dst = walk.dst.virt.addr;
		unsigned int func_bytes, num_blocks;
		unsigned int i;
		u128 last_iv;

		fpu_enabled = glue_fpu_begin(bsize, gctx->fpu_blocks_limit,
					     &walk, fpu_enabled, nbytes);
		/* Start of the last block. */
		src += nbytes / bsize - 1;
		dst += nbytes / bsize - 1;

		last_iv = *src;

		for (i = 0; i < gctx->num_funcs; i++) {
			num_blocks = gctx->funcs[i].num_blocks;
			func_bytes = bsize * num_blocks;

			if (nbytes < func_bytes)
				continue;

			/* Process multi-block batch */
			do {
				src -= num_blocks - 1;
				dst -= num_blocks - 1;

				gctx->funcs[i].fn_u.cbc(ctx, dst, src);

				nbytes -= func_bytes;
				if (nbytes < bsize)
					goto done;

				u128_xor(dst, dst, --src);
				dst--;
			} while (nbytes >= func_bytes);
		}
done:
		u128_xor(dst, dst, (u128 *)walk.iv);
		*(u128 *)walk.iv = last_iv;
		err = skcipher_walk_done(&walk, nbytes);
	}

	glue_fpu_end(fpu_enabled);
	return err;
}
EXPORT_SYMBOL_GPL(glue_cbc_decrypt_req_128bit);

int glue_ctr_req_128bit(const struct common_glue_ctx *gctx,
			struct skcipher_request *req)
{
	void *ctx = crypto_skcipher_ctx(crypto_skcipher_reqtfm(req));
	const unsigned int bsize = 128 / 8;
	struct skcipher_walk walk;
	bool fpu_enabled = false;
	unsigned int nbytes;
	int err;

	err = skcipher_walk_virt(&walk, req, false);

	while ((nbytes = walk.nbytes) >= bsize) {
		const u128 *src = walk.src.virt.addr;
		u128 *dst = walk.dst.virt.addr;
		unsigned int func_bytes, num_blocks;
		unsigned int i;
		le128 ctrblk;

		fpu_enabled = glue_fpu_begin(bsize, gctx->fpu_blocks_limit,
					     &walk, fpu_enabled, nbytes);

		be128_to_le128(&ctrblk, (be128 *)walk.iv);

		for (i = 0; i < gctx->num_funcs; i++) {
			num_blocks = gctx->funcs[i].num_blocks;
			func_bytes = bsize * num_blocks;

			if (nbytes < func_bytes)
				continue;

			/* Process multi-block batch */
			do {
				gctx->funcs[i].fn_u.ctr(ctx, dst, src, &ctrblk);
				src += num_blocks;
				dst += num_blocks;
				nbytes -= func_bytes;
			} while (nbytes >= func_bytes);

			if (nbytes < bsize)
				break;
		}

		le128_to_be128((be128 *)walk.iv, &ctrblk);
		err = skcipher_walk_done(&walk, nbytes);
	}

	glue_fpu_end(fpu_enabled);

	if (nbytes) {
		le128 ctrblk;
		u128 tmp;

		be128_to_le128(&ctrblk, (be128 *)walk.iv);
		memcpy(&tmp, walk.src.virt.addr, nbytes);
		gctx->funcs[gctx->num_funcs - 1].fn_u.ctr(ctx, &tmp, &tmp,
							  &ctrblk);
		memcpy(walk.dst.virt.addr, &tmp, nbytes);
		le128_to_be128((be128 *)walk.iv, &ctrblk);

		err = skcipher_walk_done(&walk, 0);
	}

	return err;
}
EXPORT_SYMBOL_GPL(glue_ctr_req_128bit);

static unsigned int __glue_xts_req_128bit(const struct common_glue_ctx *gctx,
					  void *ctx,
					  struct skcipher_walk *walk)
{
	const unsigned int bsize = 128 / 8;
	unsigned int nbytes = walk->nbytes;
	u128 *src = walk->src.virt.addr;
	u128 *dst = walk->dst.virt.addr;
	unsigned int num_blocks, func_bytes;
	unsigned int i;

	/* Process multi-block batch */
	for (i = 0; i < gctx->num_funcs; i++) {
		num_blocks = gctx->funcs[i].num_blocks;
		func_bytes = bsize * num_blocks;

		if (nbytes >= func_bytes) {
			do {
				gctx->funcs[i].fn_u.xts(ctx, dst, src,
							walk->iv);

				src += num_blocks;
				dst += num_blocks;
				nbytes -= func_bytes;
			} while (nbytes >= func_bytes);

			if (nbytes < bsize)
				goto done;
		}
	}

done:
	return nbytes;
}

int glue_xts_req_128bit(const struct common_glue_ctx *gctx,
			struct skcipher_request *req,
			common_glue_func_t tweak_fn, void *tweak_ctx,
			void *crypt_ctx, bool decrypt)
{
	const bool cts = (req->cryptlen % XTS_BLOCK_SIZE);
	const unsigned int bsize = 128 / 8;
	struct skcipher_request subreq;
	struct skcipher_walk walk;
	bool fpu_enabled = false;
	unsigned int nbytes, tail;
	int err;

	if (req->cryptlen < XTS_BLOCK_SIZE)
		return -EINVAL;

	if (unlikely(cts)) {
		struct crypto_skcipher *tfm = crypto_skcipher_reqtfm(req);

		tail = req->cryptlen % XTS_BLOCK_SIZE + XTS_BLOCK_SIZE;

		skcipher_request_set_tfm(&subreq, tfm);
		skcipher_request_set_callback(&subreq,
					      crypto_skcipher_get_flags(tfm),
					      NULL, NULL);
		skcipher_request_set_crypt(&subreq, req->src, req->dst,
					   req->cryptlen - tail, req->iv);
		req = &subreq;
	}

	err = skcipher_walk_virt(&walk, req, false);
	nbytes = walk.nbytes;
	if (err)
		return err;

	/* set minimum length to bsize, for tweak_fn */
	fpu_enabled = glue_fpu_begin(bsize, gctx->fpu_blocks_limit,
				     &walk, fpu_enabled,
				     nbytes < bsize ? bsize : nbytes);

	/* calculate first value of T */
	tweak_fn(tweak_ctx, walk.iv, walk.iv);

	while (nbytes) {
		nbytes = __glue_xts_req_128bit(gctx, crypt_ctx, &walk);

		err = skcipher_walk_done(&walk, nbytes);
		nbytes = walk.nbytes;
	}

	if (unlikely(cts)) {
		u8 *next_tweak, *final_tweak = req->iv;
		struct scatterlist *src, *dst;
		struct scatterlist s[2], d[2];
		le128 b[2];

		dst = src = scatterwalk_ffwd(s, req->src, req->cryptlen);
		if (req->dst != req->src)
			dst = scatterwalk_ffwd(d, req->dst, req->cryptlen);

		if (decrypt) {
			next_tweak = memcpy(b, req->iv, XTS_BLOCK_SIZE);
			gf128mul_x_ble(b, b);
		} else {
			next_tweak = req->iv;
		}

		skcipher_request_set_crypt(&subreq, src, dst, XTS_BLOCK_SIZE,
					   next_tweak);

		err = skcipher_walk_virt(&walk, req, false) ?:
		      skcipher_walk_done(&walk,
				__glue_xts_req_128bit(gctx, crypt_ctx, &walk));
		if (err)
			goto out;

		scatterwalk_map_and_copy(b, dst, 0, XTS_BLOCK_SIZE, 0);
		memcpy(b + 1, b, tail - XTS_BLOCK_SIZE);
		scatterwalk_map_and_copy(b, src, XTS_BLOCK_SIZE,
					 tail - XTS_BLOCK_SIZE, 0);
		scatterwalk_map_and_copy(b, dst, 0, tail, 1);

		skcipher_request_set_crypt(&subreq, dst, dst, XTS_BLOCK_SIZE,
					   final_tweak);

		err = skcipher_walk_virt(&walk, req, false) ?:
		      skcipher_walk_done(&walk,
				__glue_xts_req_128bit(gctx, crypt_ctx, &walk));
	}

out:
	glue_fpu_end(fpu_enabled);

	return err;
}
EXPORT_SYMBOL_GPL(glue_xts_req_128bit);

void glue_xts_crypt_128bit_one(void *ctx, u128 *dst, const u128 *src, le128 *iv,
			       common_glue_func_t fn)
{
	le128 ivblk = *iv;

	/* generate next IV */
	gf128mul_x_ble(iv, &ivblk);

	/* CC <- T xor C */
	u128_xor(dst, src, (u128 *)&ivblk);

	/* PP <- D(Key2,CC) */
	fn(ctx, (u8 *)dst, (u8 *)dst);

	/* P <- T xor PP */
	u128_xor(dst, dst, (u128 *)&ivblk);
}
EXPORT_SYMBOL_GPL(glue_xts_crypt_128bit_one);

MODULE_LICENSE("GPL");
