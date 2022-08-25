/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Cryptographic API for algorithms (i.e., low-level API).
 *
 * Copyright (c) 2006 Herbert Xu <herbert@gondor.apana.org.au>
 */
#ifndef _CRYPTO_ALGAPI_H
#define _CRYPTO_ALGAPI_H

#include <linux/crypto.h>
#include <linux/list.h>
#include <linux/kernel.h>
#include <linux/skbuff.h>

/*
 * Maximum values for blocksize and alignmask, used to allocate
 * static buffers that are big enough for any combination of
 * algs and architectures. Ciphers have a lower maximum size.
 */
#define MAX_ALGAPI_BLOCKSIZE		160
#define MAX_ALGAPI_ALIGNMASK		63
#define MAX_CIPHER_BLOCKSIZE		16
#define MAX_CIPHER_ALIGNMASK		15

struct crypto_aead;
struct crypto_instance;
struct module;
struct rtattr;
struct seq_file;

struct crypto_type {
	unsigned int (*ctxsize)(struct crypto_alg *alg, u32 type, u32 mask);
	unsigned int (*extsize)(struct crypto_alg *alg);
	int (*init)(struct crypto_tfm *tfm, u32 type, u32 mask);
	int (*init_tfm)(struct crypto_tfm *tfm);
	void (*show)(struct seq_file *m, struct crypto_alg *alg);
	int (*report)(struct sk_buff *skb, struct crypto_alg *alg);
	void (*free)(struct crypto_instance *inst);

	unsigned int type;
	unsigned int maskclear;
	unsigned int maskset;
	unsigned int tfmsize;
};

struct crypto_instance {
	struct crypto_alg alg;

	struct crypto_template *tmpl;
	struct hlist_node list;

	void *__ctx[] CRYPTO_MINALIGN_ATTR;
};

struct crypto_template {
	struct list_head list;
	struct hlist_head instances;
	struct module *module;

	struct crypto_instance *(*alloc)(struct rtattr **tb);
	void (*free)(struct crypto_instance *inst);
	int (*create)(struct crypto_template *tmpl, struct rtattr **tb);

	char name[CRYPTO_MAX_ALG_NAME];
};

struct crypto_spawn {
	struct list_head list;
	struct crypto_alg *alg;
	struct crypto_instance *inst;
	const struct crypto_type *frontend;
	u32 mask;
};

struct crypto_queue {
	struct list_head list;
	struct list_head *backlog;

	unsigned int qlen;
	unsigned int max_qlen;
};

struct scatter_walk {
	struct scatterlist *sg;
	unsigned int offset;
};

struct blkcipher_walk {
	union {
		struct {
			struct page *page;
			unsigned long offset;
		} phys;

		struct {
			u8 *page;
			u8 *addr;
		} virt;
	} src, dst;

	struct scatter_walk in;
	unsigned int nbytes;

	struct scatter_walk out;
	unsigned int total;

	void *page;
	u8 *buffer;
	u8 *iv;
	unsigned int ivsize;

	int flags;
	unsigned int walk_blocksize;
	unsigned int cipher_blocksize;
	unsigned int alignmask;
};

struct ablkcipher_walk {
	struct {
		struct page *page;
		unsigned int offset;
	} src, dst;

	struct scatter_walk	in;
	unsigned int		nbytes;
	struct scatter_walk	out;
	unsigned int		total;
	struct list_head	buffers;
	u8			*iv_buffer;
	u8			*iv;
	int			flags;
	unsigned int		blocksize;
};

extern const struct crypto_type crypto_ablkcipher_type;
extern const struct crypto_type crypto_blkcipher_type;

void crypto_mod_put(struct crypto_alg *alg);

int crypto_register_template(struct crypto_template *tmpl);
int crypto_register_templates(struct crypto_template *tmpls, int count);
void crypto_unregister_template(struct crypto_template *tmpl);
void crypto_unregister_templates(struct crypto_template *tmpls, int count);
struct crypto_template *crypto_lookup_template(const char *name);

int crypto_register_instance(struct crypto_template *tmpl,
			     struct crypto_instance *inst);
int crypto_unregister_instance(struct crypto_instance *inst);

int crypto_init_spawn(struct crypto_spawn *spawn, struct crypto_alg *alg,
		      struct crypto_instance *inst, u32 mask);
int crypto_init_spawn2(struct crypto_spawn *spawn, struct crypto_alg *alg,
		       struct crypto_instance *inst,
		       const struct crypto_type *frontend);
int crypto_grab_spawn(struct crypto_spawn *spawn, const char *name,
		      u32 type, u32 mask);

void crypto_drop_spawn(struct crypto_spawn *spawn);
struct crypto_tfm *crypto_spawn_tfm(struct crypto_spawn *spawn, u32 type,
				    u32 mask);
void *crypto_spawn_tfm2(struct crypto_spawn *spawn);

static inline void crypto_set_spawn(struct crypto_spawn *spawn,
				    struct crypto_instance *inst)
{
	spawn->inst = inst;
}

struct crypto_attr_type *crypto_get_attr_type(struct rtattr **tb);
int crypto_check_attr_type(struct rtattr **tb, u32 type);
const char *crypto_attr_alg_name(struct rtattr *rta);
struct crypto_alg *crypto_attr_alg2(struct rtattr *rta,
				    const struct crypto_type *frontend,
				    u32 type, u32 mask);

static inline struct crypto_alg *crypto_attr_alg(struct rtattr *rta,
						 u32 type, u32 mask)
{
	return crypto_attr_alg2(rta, NULL, type, mask);
}

int crypto_attr_u32(struct rtattr *rta, u32 *num);
int crypto_inst_setname(struct crypto_instance *inst, const char *name,
			struct crypto_alg *alg);
void *crypto_alloc_instance(const char *name, struct crypto_alg *alg,
			    unsigned int head);

void crypto_init_queue(struct crypto_queue *queue, unsigned int max_qlen);
int crypto_enqueue_request(struct crypto_queue *queue,
			   struct crypto_async_request *request);
struct crypto_async_request *crypto_dequeue_request(struct crypto_queue *queue);
static inline unsigned int crypto_queue_len(struct crypto_queue *queue)
{
	return queue->qlen;
}

void crypto_inc(u8 *a, unsigned int size);
void __crypto_xor(u8 *dst, const u8 *src1, const u8 *src2, unsigned int size);

static inline void crypto_xor(u8 *dst, const u8 *src, unsigned int size)
{
	if (IS_ENABLED(CONFIG_HAVE_EFFICIENT_UNALIGNED_ACCESS) &&
	    __builtin_constant_p(size) &&
	    (size % sizeof(unsigned long)) == 0) {
		unsigned long *d = (unsigned long *)dst;
		unsigned long *s = (unsigned long *)src;

		while (size > 0) {
			*d++ ^= *s++;
			size -= sizeof(unsigned long);
		}
	} else {
		__crypto_xor(dst, dst, src, size);
	}
}

static inline void crypto_xor_cpy(u8 *dst, const u8 *src1, const u8 *src2,
				  unsigned int size)
{
	if (IS_ENABLED(CONFIG_HAVE_EFFICIENT_UNALIGNED_ACCESS) &&
	    __builtin_constant_p(size) &&
	    (size % sizeof(unsigned long)) == 0) {
		unsigned long *d = (unsigned long *)dst;
		unsigned long *s1 = (unsigned long *)src1;
		unsigned long *s2 = (unsigned long *)src2;

		while (size > 0) {
			*d++ = *s1++ ^ *s2++;
			size -= sizeof(unsigned long);
		}
	} else {
		__crypto_xor(dst, src1, src2, size);
	}
}

int blkcipher_walk_done(struct blkcipher_desc *desc,
			struct blkcipher_walk *walk, int err);
int blkcipher_walk_virt(struct blkcipher_desc *desc,
			struct blkcipher_walk *walk);
int blkcipher_walk_phys(struct blkcipher_desc *desc,
			struct blkcipher_walk *walk);
int blkcipher_walk_virt_block(struct blkcipher_desc *desc,
			      struct blkcipher_walk *walk,
			      unsigned int blocksize);
int blkcipher_aead_walk_virt_block(struct blkcipher_desc *desc,
				   struct blkcipher_walk *walk,
				   struct crypto_aead *tfm,
				   unsigned int blocksize);

int ablkcipher_walk_done(struct ablkcipher_request *req,
			 struct ablkcipher_walk *walk, int err);
int ablkcipher_walk_phys(struct ablkcipher_request *req,
			 struct ablkcipher_walk *walk);
void __ablkcipher_walk_complete(struct ablkcipher_walk *walk);

static inline void *crypto_tfm_ctx_aligned(struct crypto_tfm *tfm)
{
	return PTR_ALIGN(crypto_tfm_ctx(tfm),
			 crypto_tfm_alg_alignmask(tfm) + 1);
}

static inline struct crypto_instance *crypto_tfm_alg_instance(
	struct crypto_tfm *tfm)
{
	return container_of(tfm->__crt_alg, struct crypto_instance, alg);
}

static inline void *crypto_instance_ctx(struct crypto_instance *inst)
{
	return inst->__ctx;
}

static inline struct ablkcipher_alg *crypto_ablkcipher_alg(
	struct crypto_ablkcipher *tfm)
{
	return &crypto_ablkcipher_tfm(tfm)->__crt_alg->cra_ablkcipher;
}

static inline void *crypto_ablkcipher_ctx(struct crypto_ablkcipher *tfm)
{
	return crypto_tfm_ctx(&tfm->base);
}

static inline void *crypto_ablkcipher_ctx_aligned(struct crypto_ablkcipher *tfm)
{
	return crypto_tfm_ctx_aligned(&tfm->base);
}

static inline struct crypto_blkcipher *crypto_spawn_blkcipher(
	struct crypto_spawn *spawn)
{
	u32 type = CRYPTO_ALG_TYPE_BLKCIPHER;
	u32 mask = CRYPTO_ALG_TYPE_MASK;

	return __crypto_blkcipher_cast(crypto_spawn_tfm(spawn, type, mask));
}

static inline void *crypto_blkcipher_ctx(struct crypto_blkcipher *tfm)
{
	return crypto_tfm_ctx(&tfm->base);
}

static inline void *crypto_blkcipher_ctx_aligned(struct crypto_blkcipher *tfm)
{
	return crypto_tfm_ctx_aligned(&tfm->base);
}

static inline struct crypto_cipher *crypto_spawn_cipher(
	struct crypto_spawn *spawn)
{
	u32 type = CRYPTO_ALG_TYPE_CIPHER;
	u32 mask = CRYPTO_ALG_TYPE_MASK;

	return __crypto_cipher_cast(crypto_spawn_tfm(spawn, type, mask));
}

static inline struct cipher_alg *crypto_cipher_alg(struct crypto_cipher *tfm)
{
	return &crypto_cipher_tfm(tfm)->__crt_alg->cra_cipher;
}

static inline void blkcipher_walk_init(struct blkcipher_walk *walk,
				       struct scatterlist *dst,
				       struct scatterlist *src,
				       unsigned int nbytes)
{
	walk->in.sg = src;
	walk->out.sg = dst;
	walk->total = nbytes;
}

static inline void ablkcipher_walk_init(struct ablkcipher_walk *walk,
					struct scatterlist *dst,
					struct scatterlist *src,
					unsigned int nbytes)
{
	walk->in.sg = src;
	walk->out.sg = dst;
	walk->total = nbytes;
	INIT_LIST_HEAD(&walk->buffers);
}

static inline void ablkcipher_walk_complete(struct ablkcipher_walk *walk)
{
	if (unlikely(!list_empty(&walk->buffers)))
		__ablkcipher_walk_complete(walk);
}

static inline struct crypto_async_request *crypto_get_backlog(
	struct crypto_queue *queue)
{
	return queue->backlog == &queue->list ? NULL :
	       container_of(queue->backlog, struct crypto_async_request, list);
}

static inline int ablkcipher_enqueue_request(struct crypto_queue *queue,
					     struct ablkcipher_request *request)
{
	return crypto_enqueue_request(queue, &request->base);
}

static inline struct ablkcipher_request *ablkcipher_dequeue_request(
	struct crypto_queue *queue)
{
	return ablkcipher_request_cast(crypto_dequeue_request(queue));
}

static inline void *ablkcipher_request_ctx(struct ablkcipher_request *req)
{
	return req->__ctx;
}

static inline struct crypto_alg *crypto_get_attr_alg(struct rtattr **tb,
						     u32 type, u32 mask)
{
	return crypto_attr_alg(tb[1], type, mask);
}

static inline int crypto_requires_off(u32 type, u32 mask, u32 off)
{
	return (type ^ off) & mask & off;
}

/*
 * Returns CRYPTO_ALG_ASYNC if type/mask requires the use of sync algorithms.
 * Otherwise returns zero.
 */
static inline int crypto_requires_sync(u32 type, u32 mask)
{
	return crypto_requires_off(type, mask, CRYPTO_ALG_ASYNC);
}

noinline unsigned long __crypto_memneq(const void *a, const void *b, size_t size);

/**
 * crypto_memneq - Compare two areas of memory without leaking
 *		   timing information.
 *
 * @a: One area of memory
 * @b: Another area of memory
 * @size: The size of the area.
 *
 * Returns 0 when data is equal, 1 otherwise.
 */
static inline int crypto_memneq(const void *a, const void *b, size_t size)
{
	return __crypto_memneq(a, b, size) != 0UL ? 1 : 0;
}

static inline void crypto_yield(u32 flags)
{
	if (flags & CRYPTO_TFM_REQ_MAY_SLEEP)
		cond_resched();
}

int crypto_register_notifier(struct notifier_block *nb);
int crypto_unregister_notifier(struct notifier_block *nb);

/* Crypto notification events. */
enum {
	CRYPTO_MSG_ALG_REQUEST,
	CRYPTO_MSG_ALG_REGISTER,
	CRYPTO_MSG_ALG_LOADED,
};

#endif	/* _CRYPTO_ALGAPI_H */
