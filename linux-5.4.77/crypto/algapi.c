// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Cryptographic API for algorithms (i.e., low-level API).
 *
 * Copyright (c) 2006 Herbert Xu <herbert@gondor.apana.org.au>
 */

#include <crypto/algapi.h>
#include <linux/err.h>
#include <linux/errno.h>
#include <linux/fips.h>
#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/list.h>
#include <linux/module.h>
#include <linux/rtnetlink.h>
#include <linux/slab.h>
#include <linux/string.h>

#include "internal.h"

static LIST_HEAD(crypto_template_list);

static inline void crypto_check_module_sig(struct module *mod)
{
	if (fips_enabled && mod && !module_sig_ok(mod))
		panic("Module %s signature verification failed in FIPS mode\n",
		      module_name(mod));
}

static int crypto_check_alg(struct crypto_alg *alg)
{
	crypto_check_module_sig(alg->cra_module);

	if (!alg->cra_name[0] || !alg->cra_driver_name[0])
		return -EINVAL;

	if (alg->cra_alignmask & (alg->cra_alignmask + 1))
		return -EINVAL;

	/* General maximums for all algs. */
	if (alg->cra_alignmask > MAX_ALGAPI_ALIGNMASK)
		return -EINVAL;

	if (alg->cra_blocksize > MAX_ALGAPI_BLOCKSIZE)
		return -EINVAL;

	/* Lower maximums for specific alg types. */
	if (!alg->cra_type && (alg->cra_flags & CRYPTO_ALG_TYPE_MASK) ==
			       CRYPTO_ALG_TYPE_CIPHER) {
		if (alg->cra_alignmask > MAX_CIPHER_ALIGNMASK)
			return -EINVAL;

		if (alg->cra_blocksize > MAX_CIPHER_BLOCKSIZE)
			return -EINVAL;
	}

	if (alg->cra_priority < 0)
		return -EINVAL;

	refcount_set(&alg->cra_refcnt, 1);

	return 0;
}

static void crypto_free_instance(struct crypto_instance *inst)
{
	if (!inst->alg.cra_type->free) {
		inst->tmpl->free(inst);
		return;
	}

	inst->alg.cra_type->free(inst);
}

static void crypto_destroy_instance(struct crypto_alg *alg)
{
	struct crypto_instance *inst = (void *)alg;
	struct crypto_template *tmpl = inst->tmpl;

	crypto_free_instance(inst);
	crypto_tmpl_put(tmpl);
}

static struct list_head *crypto_more_spawns(struct crypto_alg *alg,
					    struct list_head *stack,
					    struct list_head *top,
					    struct list_head *secondary_spawns)
{
	struct crypto_spawn *spawn, *n;

	spawn = list_first_entry_or_null(stack, struct crypto_spawn, list);
	if (!spawn)
		return NULL;

	n = list_next_entry(spawn, list);

	if (spawn->alg && &n->list != stack && !n->alg)
		n->alg = (n->list.next == stack) ? alg :
			 &list_next_entry(n, list)->inst->alg;

	list_move(&spawn->list, secondary_spawns);

	return &n->list == stack ? top : &n->inst->alg.cra_users;
}

static void crypto_remove_instance(struct crypto_instance *inst,
				   struct list_head *list)
{
	struct crypto_template *tmpl = inst->tmpl;

	if (crypto_is_dead(&inst->alg))
		return;

	inst->alg.cra_flags |= CRYPTO_ALG_DEAD;
	if (hlist_unhashed(&inst->list))
		return;

	if (!tmpl || !crypto_tmpl_get(tmpl))
		return;

	list_move(&inst->alg.cra_list, list);
	hlist_del(&inst->list);
	inst->alg.cra_destroy = crypto_destroy_instance;

	BUG_ON(!list_empty(&inst->alg.cra_users));
}

void crypto_remove_spawns(struct crypto_alg *alg, struct list_head *list,
			  struct crypto_alg *nalg)
{
	u32 new_type = (nalg ?: alg)->cra_flags;
	struct crypto_spawn *spawn, *n;
	LIST_HEAD(secondary_spawns);
	struct list_head *spawns;
	LIST_HEAD(stack);
	LIST_HEAD(top);

	spawns = &alg->cra_users;
	list_for_each_entry_safe(spawn, n, spawns, list) {
		if ((spawn->alg->cra_flags ^ new_type) & spawn->mask)
			continue;

		list_move(&spawn->list, &top);
	}

	spawns = &top;
	do {
		while (!list_empty(spawns)) {
			struct crypto_instance *inst;

			spawn = list_first_entry(spawns, struct crypto_spawn,
						 list);
			inst = spawn->inst;

			BUG_ON(&inst->alg == alg);

			list_move(&spawn->list, &stack);

			if (&inst->alg == nalg)
				break;

			spawn->alg = NULL;
			spawns = &inst->alg.cra_users;

			/*
			 * We may encounter an unregistered instance here, since
			 * an instance's spawns are set up prior to the instance
			 * being registered.  An unregistered instance will have
			 * NULL ->cra_users.next, since ->cra_users isn't
			 * properly initialized until registration.  But an
			 * unregistered instance cannot have any users, so treat
			 * it the same as ->cra_users being empty.
			 */
			if (spawns->next == NULL)
				break;
		}
	} while ((spawns = crypto_more_spawns(alg, &stack, &top,
					      &secondary_spawns)));

	list_for_each_entry_safe(spawn, n, &secondary_spawns, list) {
		if (spawn->alg)
			list_move(&spawn->list, &spawn->alg->cra_users);
		else
			crypto_remove_instance(spawn->inst, list);
	}
}
EXPORT_SYMBOL_GPL(crypto_remove_spawns);

static struct crypto_larval *__crypto_register_alg(struct crypto_alg *alg)
{
	struct crypto_alg *q;
	struct crypto_larval *larval;
	int ret = -EAGAIN;

	if (crypto_is_dead(alg))
		goto err;

	INIT_LIST_HEAD(&alg->cra_users);

	/* No cheating! */
	alg->cra_flags &= ~CRYPTO_ALG_TESTED;

	ret = -EEXIST;

	list_for_each_entry(q, &crypto_alg_list, cra_list) {
		if (q == alg)
			goto err;

		if (crypto_is_moribund(q))
			continue;

		if (crypto_is_larval(q)) {
			if (!strcmp(alg->cra_driver_name, q->cra_driver_name))
				goto err;
			continue;
		}

		if (!strcmp(q->cra_driver_name, alg->cra_name) ||
		    !strcmp(q->cra_name, alg->cra_driver_name))
			goto err;
	}

	larval = crypto_larval_alloc(alg->cra_name,
				     alg->cra_flags | CRYPTO_ALG_TESTED, 0);
	if (IS_ERR(larval))
		goto out;

	ret = -ENOENT;
	larval->adult = crypto_mod_get(alg);
	if (!larval->adult)
		goto free_larval;

	refcount_set(&larval->alg.cra_refcnt, 1);
	memcpy(larval->alg.cra_driver_name, alg->cra_driver_name,
	       CRYPTO_MAX_ALG_NAME);
	larval->alg.cra_priority = alg->cra_priority;

	list_add(&alg->cra_list, &crypto_alg_list);
	list_add(&larval->alg.cra_list, &crypto_alg_list);

	crypto_stats_init(alg);

out:
	return larval;

free_larval:
	kfree(larval);
err:
	larval = ERR_PTR(ret);
	goto out;
}

void crypto_alg_tested(const char *name, int err)
{
	struct crypto_larval *test;
	struct crypto_alg *alg;
	struct crypto_alg *q;
	LIST_HEAD(list);
	bool best;

	down_write(&crypto_alg_sem);
	list_for_each_entry(q, &crypto_alg_list, cra_list) {
		if (crypto_is_moribund(q) || !crypto_is_larval(q))
			continue;

		test = (struct crypto_larval *)q;

		if (!strcmp(q->cra_driver_name, name))
			goto found;
	}

	pr_err("alg: Unexpected test result for %s: %d\n", name, err);
	goto unlock;

found:
	q->cra_flags |= CRYPTO_ALG_DEAD;
	alg = test->adult;
	if (err || list_empty(&alg->cra_list))
		goto complete;

	alg->cra_flags |= CRYPTO_ALG_TESTED;

	/* Only satisfy larval waiters if we are the best. */
	best = true;
	list_for_each_entry(q, &crypto_alg_list, cra_list) {
		if (crypto_is_moribund(q) || !crypto_is_larval(q))
			continue;

		if (strcmp(alg->cra_name, q->cra_name))
			continue;

		if (q->cra_priority > alg->cra_priority) {
			best = false;
			break;
		}
	}

	list_for_each_entry(q, &crypto_alg_list, cra_list) {
		if (q == alg)
			continue;

		if (crypto_is_moribund(q))
			continue;

		if (crypto_is_larval(q)) {
			struct crypto_larval *larval = (void *)q;

			/*
			 * Check to see if either our generic name or
			 * specific name can satisfy the name requested
			 * by the larval entry q.
			 */
			if (strcmp(alg->cra_name, q->cra_name) &&
			    strcmp(alg->cra_driver_name, q->cra_name))
				continue;

			if (larval->adult)
				continue;
			if ((q->cra_flags ^ alg->cra_flags) & larval->mask)
				continue;

			if (best && crypto_mod_get(alg))
				larval->adult = alg;
			else
				larval->adult = ERR_PTR(-EAGAIN);

			continue;
		}

		if (strcmp(alg->cra_name, q->cra_name))
			continue;

		if (strcmp(alg->cra_driver_name, q->cra_driver_name) &&
		    q->cra_priority > alg->cra_priority)
			continue;

		crypto_remove_spawns(q, &list, alg);
	}

complete:
	complete_all(&test->completion);

unlock:
	up_write(&crypto_alg_sem);

	crypto_remove_final(&list);
}
EXPORT_SYMBOL_GPL(crypto_alg_tested);

void crypto_remove_final(struct list_head *list)
{
	struct crypto_alg *alg;
	struct crypto_alg *n;

	list_for_each_entry_safe(alg, n, list, cra_list) {
		list_del_init(&alg->cra_list);
		crypto_alg_put(alg);
	}
}
EXPORT_SYMBOL_GPL(crypto_remove_final);

static void crypto_wait_for_test(struct crypto_larval *larval)
{
	int err;

	err = crypto_probing_notify(CRYPTO_MSG_ALG_REGISTER, larval->adult);
	if (err != NOTIFY_STOP) {
		if (WARN_ON(err != NOTIFY_DONE))
			goto out;
		crypto_alg_tested(larval->alg.cra_driver_name, 0);
	}

	err = wait_for_completion_killable(&larval->completion);
	WARN_ON(err);
	if (!err)
		crypto_notify(CRYPTO_MSG_ALG_LOADED, larval);

out:
	crypto_larval_kill(&larval->alg);
}

int crypto_register_alg(struct crypto_alg *alg)
{
	struct crypto_larval *larval;
	int err;

	alg->cra_flags &= ~CRYPTO_ALG_DEAD;
	err = crypto_check_alg(alg);
	if (err)
		return err;

	down_write(&crypto_alg_sem);
	larval = __crypto_register_alg(alg);
	up_write(&crypto_alg_sem);

	if (IS_ERR(larval))
		return PTR_ERR(larval);

	crypto_wait_for_test(larval);
	return 0;
}
EXPORT_SYMBOL_GPL(crypto_register_alg);

static int crypto_remove_alg(struct crypto_alg *alg, struct list_head *list)
{
	if (unlikely(list_empty(&alg->cra_list)))
		return -ENOENT;

	alg->cra_flags |= CRYPTO_ALG_DEAD;

	list_del_init(&alg->cra_list);
	crypto_remove_spawns(alg, list, NULL);

	return 0;
}

int crypto_unregister_alg(struct crypto_alg *alg)
{
	int ret;
	LIST_HEAD(list);

	down_write(&crypto_alg_sem);
	ret = crypto_remove_alg(alg, &list);
	up_write(&crypto_alg_sem);

	if (ret)
		return ret;

	BUG_ON(refcount_read(&alg->cra_refcnt) != 1);
	if (alg->cra_destroy)
		alg->cra_destroy(alg);

	crypto_remove_final(&list);
	return 0;
}
EXPORT_SYMBOL_GPL(crypto_unregister_alg);

int crypto_register_algs(struct crypto_alg *algs, int count)
{
	int i, ret;

	for (i = 0; i < count; i++) {
		ret = crypto_register_alg(&algs[i]);
		if (ret)
			goto err;
	}

	return 0;

err:
	for (--i; i >= 0; --i)
		crypto_unregister_alg(&algs[i]);

	return ret;
}
EXPORT_SYMBOL_GPL(crypto_register_algs);

int crypto_unregister_algs(struct crypto_alg *algs, int count)
{
	int i, ret;

	for (i = 0; i < count; i++) {
		ret = crypto_unregister_alg(&algs[i]);
		if (ret)
			pr_err("Failed to unregister %s %s: %d\n",
			       algs[i].cra_driver_name, algs[i].cra_name, ret);
	}

	return 0;
}
EXPORT_SYMBOL_GPL(crypto_unregister_algs);

int crypto_register_template(struct crypto_template *tmpl)
{
	struct crypto_template *q;
	int err = -EEXIST;

	down_write(&crypto_alg_sem);

	crypto_check_module_sig(tmpl->module);

	list_for_each_entry(q, &crypto_template_list, list) {
		if (q == tmpl)
			goto out;
	}

	list_add(&tmpl->list, &crypto_template_list);
	err = 0;
out:
	up_write(&crypto_alg_sem);
	return err;
}
EXPORT_SYMBOL_GPL(crypto_register_template);

int crypto_register_templates(struct crypto_template *tmpls, int count)
{
	int i, err;

	for (i = 0; i < count; i++) {
		err = crypto_register_template(&tmpls[i]);
		if (err)
			goto out;
	}
	return 0;

out:
	for (--i; i >= 0; --i)
		crypto_unregister_template(&tmpls[i]);
	return err;
}
EXPORT_SYMBOL_GPL(crypto_register_templates);

void crypto_unregister_template(struct crypto_template *tmpl)
{
	struct crypto_instance *inst;
	struct hlist_node *n;
	struct hlist_head *list;
	LIST_HEAD(users);

	down_write(&crypto_alg_sem);

	BUG_ON(list_empty(&tmpl->list));
	list_del_init(&tmpl->list);

	list = &tmpl->instances;
	hlist_for_each_entry(inst, list, list) {
		int err = crypto_remove_alg(&inst->alg, &users);

		BUG_ON(err);
	}

	up_write(&crypto_alg_sem);

	hlist_for_each_entry_safe(inst, n, list, list) {
		BUG_ON(refcount_read(&inst->alg.cra_refcnt) != 1);
		crypto_free_instance(inst);
	}
	crypto_remove_final(&users);
}
EXPORT_SYMBOL_GPL(crypto_unregister_template);

void crypto_unregister_templates(struct crypto_template *tmpls, int count)
{
	int i;

	for (i = count - 1; i >= 0; --i)
		crypto_unregister_template(&tmpls[i]);
}
EXPORT_SYMBOL_GPL(crypto_unregister_templates);

static struct crypto_template *__crypto_lookup_template(const char *name)
{
	struct crypto_template *q, *tmpl = NULL;

	down_read(&crypto_alg_sem);
	list_for_each_entry(q, &crypto_template_list, list) {
		if (strcmp(q->name, name))
			continue;
		if (unlikely(!crypto_tmpl_get(q)))
			continue;

		tmpl = q;
		break;
	}
	up_read(&crypto_alg_sem);

	return tmpl;
}

struct crypto_template *crypto_lookup_template(const char *name)
{
	return try_then_request_module(__crypto_lookup_template(name),
				       "crypto-%s", name);
}
EXPORT_SYMBOL_GPL(crypto_lookup_template);

int crypto_register_instance(struct crypto_template *tmpl,
			     struct crypto_instance *inst)
{
	struct crypto_larval *larval;
	int err;

	err = crypto_check_alg(&inst->alg);
	if (err)
		return err;

	inst->alg.cra_module = tmpl->module;
	inst->alg.cra_flags |= CRYPTO_ALG_INSTANCE;

	down_write(&crypto_alg_sem);

	larval = __crypto_register_alg(&inst->alg);
	if (IS_ERR(larval))
		goto unlock;

	hlist_add_head(&inst->list, &tmpl->instances);
	inst->tmpl = tmpl;

unlock:
	up_write(&crypto_alg_sem);

	err = PTR_ERR(larval);
	if (IS_ERR(larval))
		goto err;

	crypto_wait_for_test(larval);
	err = 0;

err:
	return err;
}
EXPORT_SYMBOL_GPL(crypto_register_instance);

int crypto_unregister_instance(struct crypto_instance *inst)
{
	LIST_HEAD(list);

	down_write(&crypto_alg_sem);

	crypto_remove_spawns(&inst->alg, &list, NULL);
	crypto_remove_instance(inst, &list);

	up_write(&crypto_alg_sem);

	crypto_remove_final(&list);

	return 0;
}
EXPORT_SYMBOL_GPL(crypto_unregister_instance);

int crypto_init_spawn(struct crypto_spawn *spawn, struct crypto_alg *alg,
		      struct crypto_instance *inst, u32 mask)
{
	int err = -EAGAIN;

	if (WARN_ON_ONCE(inst == NULL))
		return -EINVAL;

	spawn->inst = inst;
	spawn->mask = mask;

	down_write(&crypto_alg_sem);
	if (!crypto_is_moribund(alg)) {
		list_add(&spawn->list, &alg->cra_users);
		spawn->alg = alg;
		err = 0;
	}
	up_write(&crypto_alg_sem);

	return err;
}
EXPORT_SYMBOL_GPL(crypto_init_spawn);

int crypto_init_spawn2(struct crypto_spawn *spawn, struct crypto_alg *alg,
		       struct crypto_instance *inst,
		       const struct crypto_type *frontend)
{
	int err = -EINVAL;

	if ((alg->cra_flags ^ frontend->type) & frontend->maskset)
		goto out;

	spawn->frontend = frontend;
	err = crypto_init_spawn(spawn, alg, inst, frontend->maskset);

out:
	return err;
}
EXPORT_SYMBOL_GPL(crypto_init_spawn2);

int crypto_grab_spawn(struct crypto_spawn *spawn, const char *name,
		      u32 type, u32 mask)
{
	struct crypto_alg *alg;
	int err;

	alg = crypto_find_alg(name, spawn->frontend, type, mask);
	if (IS_ERR(alg))
		return PTR_ERR(alg);

	err = crypto_init_spawn(spawn, alg, spawn->inst, mask);
	crypto_mod_put(alg);
	return err;
}
EXPORT_SYMBOL_GPL(crypto_grab_spawn);

void crypto_drop_spawn(struct crypto_spawn *spawn)
{
	down_write(&crypto_alg_sem);
	if (spawn->alg)
		list_del(&spawn->list);
	up_write(&crypto_alg_sem);
}
EXPORT_SYMBOL_GPL(crypto_drop_spawn);

static struct crypto_alg *crypto_spawn_alg(struct crypto_spawn *spawn)
{
	struct crypto_alg *alg;

	down_read(&crypto_alg_sem);
	alg = spawn->alg;
	if (alg && !crypto_mod_get(alg)) {
		alg->cra_flags |= CRYPTO_ALG_DYING;
		alg = NULL;
	}
	up_read(&crypto_alg_sem);

	return alg ?: ERR_PTR(-EAGAIN);
}

struct crypto_tfm *crypto_spawn_tfm(struct crypto_spawn *spawn, u32 type,
				    u32 mask)
{
	struct crypto_alg *alg;
	struct crypto_tfm *tfm;

	alg = crypto_spawn_alg(spawn);
	if (IS_ERR(alg))
		return ERR_CAST(alg);

	tfm = ERR_PTR(-EINVAL);
	if (unlikely((alg->cra_flags ^ type) & mask))
		goto out_put_alg;

	tfm = __crypto_alloc_tfm(alg, type, mask);
	if (IS_ERR(tfm))
		goto out_put_alg;

	return tfm;

out_put_alg:
	crypto_mod_put(alg);
	return tfm;
}
EXPORT_SYMBOL_GPL(crypto_spawn_tfm);

void *crypto_spawn_tfm2(struct crypto_spawn *spawn)
{
	struct crypto_alg *alg;
	struct crypto_tfm *tfm;

	alg = crypto_spawn_alg(spawn);
	if (IS_ERR(alg))
		return ERR_CAST(alg);

	tfm = crypto_create_tfm(alg, spawn->frontend);
	if (IS_ERR(tfm))
		goto out_put_alg;

	return tfm;

out_put_alg:
	crypto_mod_put(alg);
	return tfm;
}
EXPORT_SYMBOL_GPL(crypto_spawn_tfm2);

int crypto_register_notifier(struct notifier_block *nb)
{
	return blocking_notifier_chain_register(&crypto_chain, nb);
}
EXPORT_SYMBOL_GPL(crypto_register_notifier);

int crypto_unregister_notifier(struct notifier_block *nb)
{
	return blocking_notifier_chain_unregister(&crypto_chain, nb);
}
EXPORT_SYMBOL_GPL(crypto_unregister_notifier);

struct crypto_attr_type *crypto_get_attr_type(struct rtattr **tb)
{
	struct rtattr *rta = tb[0];
	struct crypto_attr_type *algt;

	if (!rta)
		return ERR_PTR(-ENOENT);
	if (RTA_PAYLOAD(rta) < sizeof(*algt))
		return ERR_PTR(-EINVAL);
	if (rta->rta_type != CRYPTOA_TYPE)
		return ERR_PTR(-EINVAL);

	algt = RTA_DATA(rta);

	return algt;
}
EXPORT_SYMBOL_GPL(crypto_get_attr_type);

int crypto_check_attr_type(struct rtattr **tb, u32 type)
{
	struct crypto_attr_type *algt;

	algt = crypto_get_attr_type(tb);
	if (IS_ERR(algt))
		return PTR_ERR(algt);

	if ((algt->type ^ type) & algt->mask)
		return -EINVAL;

	return 0;
}
EXPORT_SYMBOL_GPL(crypto_check_attr_type);

const char *crypto_attr_alg_name(struct rtattr *rta)
{
	struct crypto_attr_alg *alga;

	if (!rta)
		return ERR_PTR(-ENOENT);
	if (RTA_PAYLOAD(rta) < sizeof(*alga))
		return ERR_PTR(-EINVAL);
	if (rta->rta_type != CRYPTOA_ALG)
		return ERR_PTR(-EINVAL);

	alga = RTA_DATA(rta);
	alga->name[CRYPTO_MAX_ALG_NAME - 1] = 0;

	return alga->name;
}
EXPORT_SYMBOL_GPL(crypto_attr_alg_name);

struct crypto_alg *crypto_attr_alg2(struct rtattr *rta,
				    const struct crypto_type *frontend,
				    u32 type, u32 mask)
{
	const char *name;

	name = crypto_attr_alg_name(rta);
	if (IS_ERR(name))
		return ERR_CAST(name);

	return crypto_find_alg(name, frontend, type, mask);
}
EXPORT_SYMBOL_GPL(crypto_attr_alg2);

int crypto_attr_u32(struct rtattr *rta, u32 *num)
{
	struct crypto_attr_u32 *nu32;

	if (!rta)
		return -ENOENT;
	if (RTA_PAYLOAD(rta) < sizeof(*nu32))
		return -EINVAL;
	if (rta->rta_type != CRYPTOA_U32)
		return -EINVAL;

	nu32 = RTA_DATA(rta);
	*num = nu32->num;

	return 0;
}
EXPORT_SYMBOL_GPL(crypto_attr_u32);

int crypto_inst_setname(struct crypto_instance *inst, const char *name,
			struct crypto_alg *alg)
{
	if (snprintf(inst->alg.cra_name, CRYPTO_MAX_ALG_NAME, "%s(%s)", name,
		     alg->cra_name) >= CRYPTO_MAX_ALG_NAME)
		return -ENAMETOOLONG;

	if (snprintf(inst->alg.cra_driver_name, CRYPTO_MAX_ALG_NAME, "%s(%s)",
		     name, alg->cra_driver_name) >= CRYPTO_MAX_ALG_NAME)
		return -ENAMETOOLONG;

	return 0;
}
EXPORT_SYMBOL_GPL(crypto_inst_setname);

void *crypto_alloc_instance(const char *name, struct crypto_alg *alg,
			    unsigned int head)
{
	struct crypto_instance *inst;
	char *p;
	int err;

	p = kzalloc(head + sizeof(*inst) + sizeof(struct crypto_spawn),
		    GFP_KERNEL);
	if (!p)
		return ERR_PTR(-ENOMEM);

	inst = (void *)(p + head);

	err = crypto_inst_setname(inst, name, alg);
	if (err)
		goto err_free_inst;

	return p;

err_free_inst:
	kfree(p);
	return ERR_PTR(err);
}
EXPORT_SYMBOL_GPL(crypto_alloc_instance);

void crypto_init_queue(struct crypto_queue *queue, unsigned int max_qlen)
{
	INIT_LIST_HEAD(&queue->list);
	queue->backlog = &queue->list;
	queue->qlen = 0;
	queue->max_qlen = max_qlen;
}
EXPORT_SYMBOL_GPL(crypto_init_queue);

int crypto_enqueue_request(struct crypto_queue *queue,
			   struct crypto_async_request *request)
{
	int err = -EINPROGRESS;

	if (unlikely(queue->qlen >= queue->max_qlen)) {
		if (!(request->flags & CRYPTO_TFM_REQ_MAY_BACKLOG)) {
			err = -ENOSPC;
			goto out;
		}
		err = -EBUSY;
		if (queue->backlog == &queue->list)
			queue->backlog = &request->list;
	}

	queue->qlen++;
	list_add_tail(&request->list, &queue->list);

out:
	return err;
}
EXPORT_SYMBOL_GPL(crypto_enqueue_request);

struct crypto_async_request *crypto_dequeue_request(struct crypto_queue *queue)
{
	struct list_head *request;

	if (unlikely(!queue->qlen))
		return NULL;

	queue->qlen--;

	if (queue->backlog != &queue->list)
		queue->backlog = queue->backlog->next;

	request = queue->list.next;
	list_del(request);

	return list_entry(request, struct crypto_async_request, list);
}
EXPORT_SYMBOL_GPL(crypto_dequeue_request);

static inline void crypto_inc_byte(u8 *a, unsigned int size)
{
	u8 *b = (a + size);
	u8 c;

	for (; size; size--) {
		c = *--b + 1;
		*b = c;
		if (c)
			break;
	}
}

void crypto_inc(u8 *a, unsigned int size)
{
	__be32 *b = (__be32 *)(a + size);
	u32 c;

	if (IS_ENABLED(CONFIG_HAVE_EFFICIENT_UNALIGNED_ACCESS) ||
	    IS_ALIGNED((unsigned long)b, __alignof__(*b)))
		for (; size >= 4; size -= 4) {
			c = be32_to_cpu(*--b) + 1;
			*b = cpu_to_be32(c);
			if (likely(c))
				return;
		}

	crypto_inc_byte(a, size);
}
EXPORT_SYMBOL_GPL(crypto_inc);

void __crypto_xor(u8 *dst, const u8 *src1, const u8 *src2, unsigned int len)
{
	int relalign = 0;

	if (!IS_ENABLED(CONFIG_HAVE_EFFICIENT_UNALIGNED_ACCESS)) {
		int size = sizeof(unsigned long);
		int d = (((unsigned long)dst ^ (unsigned long)src1) |
			 ((unsigned long)dst ^ (unsigned long)src2)) &
			(size - 1);

		relalign = d ? 1 << __ffs(d) : size;

		/*
		 * If we care about alignment, process as many bytes as
		 * needed to advance dst and src to values whose alignments
		 * equal their relative alignment. This will allow us to
		 * process the remainder of the input using optimal strides.
		 */
		while (((unsigned long)dst & (relalign - 1)) && len > 0) {
			*dst++ = *src1++ ^ *src2++;
			len--;
		}
	}

	while (IS_ENABLED(CONFIG_64BIT) && len >= 8 && !(relalign & 7)) {
		*(u64 *)dst = *(u64 *)src1 ^  *(u64 *)src2;
		dst += 8;
		src1 += 8;
		src2 += 8;
		len -= 8;
	}

	while (len >= 4 && !(relalign & 3)) {
		*(u32 *)dst = *(u32 *)src1 ^ *(u32 *)src2;
		dst += 4;
		src1 += 4;
		src2 += 4;
		len -= 4;
	}

	while (len >= 2 && !(relalign & 1)) {
		*(u16 *)dst = *(u16 *)src1 ^ *(u16 *)src2;
		dst += 2;
		src1 += 2;
		src2 += 2;
		len -= 2;
	}

	while (len--)
		*dst++ = *src1++ ^ *src2++;
}
EXPORT_SYMBOL_GPL(__crypto_xor);

unsigned int crypto_alg_extsize(struct crypto_alg *alg)
{
	return alg->cra_ctxsize +
	       (alg->cra_alignmask & ~(crypto_tfm_ctx_alignment() - 1));
}
EXPORT_SYMBOL_GPL(crypto_alg_extsize);

int crypto_type_has_alg(const char *name, const struct crypto_type *frontend,
			u32 type, u32 mask)
{
	int ret = 0;
	struct crypto_alg *alg = crypto_find_alg(name, frontend, type, mask);

	if (!IS_ERR(alg)) {
		crypto_mod_put(alg);
		ret = 1;
	}

	return ret;
}
EXPORT_SYMBOL_GPL(crypto_type_has_alg);

#ifdef CONFIG_CRYPTO_STATS
void crypto_stats_init(struct crypto_alg *alg)
{
	memset(&alg->stats, 0, sizeof(alg->stats));
}
EXPORT_SYMBOL_GPL(crypto_stats_init);

void crypto_stats_get(struct crypto_alg *alg)
{
	crypto_alg_get(alg);
}
EXPORT_SYMBOL_GPL(crypto_stats_get);

void crypto_stats_ablkcipher_encrypt(unsigned int nbytes, int ret,
				     struct crypto_alg *alg)
{
	if (ret && ret != -EINPROGRESS && ret != -EBUSY) {
		atomic64_inc(&alg->stats.cipher.err_cnt);
	} else {
		atomic64_inc(&alg->stats.cipher.encrypt_cnt);
		atomic64_add(nbytes, &alg->stats.cipher.encrypt_tlen);
	}
	crypto_alg_put(alg);
}
EXPORT_SYMBOL_GPL(crypto_stats_ablkcipher_encrypt);

void crypto_stats_ablkcipher_decrypt(unsigned int nbytes, int ret,
				     struct crypto_alg *alg)
{
	if (ret && ret != -EINPROGRESS && ret != -EBUSY) {
		atomic64_inc(&alg->stats.cipher.err_cnt);
	} else {
		atomic64_inc(&alg->stats.cipher.decrypt_cnt);
		atomic64_add(nbytes, &alg->stats.cipher.decrypt_tlen);
	}
	crypto_alg_put(alg);
}
EXPORT_SYMBOL_GPL(crypto_stats_ablkcipher_decrypt);

void crypto_stats_aead_encrypt(unsigned int cryptlen, struct crypto_alg *alg,
			       int ret)
{
	if (ret && ret != -EINPROGRESS && ret != -EBUSY) {
		atomic64_inc(&alg->stats.aead.err_cnt);
	} else {
		atomic64_inc(&alg->stats.aead.encrypt_cnt);
		atomic64_add(cryptlen, &alg->stats.aead.encrypt_tlen);
	}
	crypto_alg_put(alg);
}
EXPORT_SYMBOL_GPL(crypto_stats_aead_encrypt);

void crypto_stats_aead_decrypt(unsigned int cryptlen, struct crypto_alg *alg,
			       int ret)
{
	if (ret && ret != -EINPROGRESS && ret != -EBUSY) {
		atomic64_inc(&alg->stats.aead.err_cnt);
	} else {
		atomic64_inc(&alg->stats.aead.decrypt_cnt);
		atomic64_add(cryptlen, &alg->stats.aead.decrypt_tlen);
	}
	crypto_alg_put(alg);
}
EXPORT_SYMBOL_GPL(crypto_stats_aead_decrypt);

void crypto_stats_akcipher_encrypt(unsigned int src_len, int ret,
				   struct crypto_alg *alg)
{
	if (ret && ret != -EINPROGRESS && ret != -EBUSY) {
		atomic64_inc(&alg->stats.akcipher.err_cnt);
	} else {
		atomic64_inc(&alg->stats.akcipher.encrypt_cnt);
		atomic64_add(src_len, &alg->stats.akcipher.encrypt_tlen);
	}
	crypto_alg_put(alg);
}
EXPORT_SYMBOL_GPL(crypto_stats_akcipher_encrypt);

void crypto_stats_akcipher_decrypt(unsigned int src_len, int ret,
				   struct crypto_alg *alg)
{
	if (ret && ret != -EINPROGRESS && ret != -EBUSY) {
		atomic64_inc(&alg->stats.akcipher.err_cnt);
	} else {
		atomic64_inc(&alg->stats.akcipher.decrypt_cnt);
		atomic64_add(src_len, &alg->stats.akcipher.decrypt_tlen);
	}
	crypto_alg_put(alg);
}
EXPORT_SYMBOL_GPL(crypto_stats_akcipher_decrypt);

void crypto_stats_akcipher_sign(int ret, struct crypto_alg *alg)
{
	if (ret && ret != -EINPROGRESS && ret != -EBUSY)
		atomic64_inc(&alg->stats.akcipher.err_cnt);
	else
		atomic64_inc(&alg->stats.akcipher.sign_cnt);
	crypto_alg_put(alg);
}
EXPORT_SYMBOL_GPL(crypto_stats_akcipher_sign);

void crypto_stats_akcipher_verify(int ret, struct crypto_alg *alg)
{
	if (ret && ret != -EINPROGRESS && ret != -EBUSY)
		atomic64_inc(&alg->stats.akcipher.err_cnt);
	else
		atomic64_inc(&alg->stats.akcipher.verify_cnt);
	crypto_alg_put(alg);
}
EXPORT_SYMBOL_GPL(crypto_stats_akcipher_verify);

void crypto_stats_compress(unsigned int slen, int ret, struct crypto_alg *alg)
{
	if (ret && ret != -EINPROGRESS && ret != -EBUSY) {
		atomic64_inc(&alg->stats.compress.err_cnt);
	} else {
		atomic64_inc(&alg->stats.compress.compress_cnt);
		atomic64_add(slen, &alg->stats.compress.compress_tlen);
	}
	crypto_alg_put(alg);
}
EXPORT_SYMBOL_GPL(crypto_stats_compress);

void crypto_stats_decompress(unsigned int slen, int ret, struct crypto_alg *alg)
{
	if (ret && ret != -EINPROGRESS && ret != -EBUSY) {
		atomic64_inc(&alg->stats.compress.err_cnt);
	} else {
		atomic64_inc(&alg->stats.compress.decompress_cnt);
		atomic64_add(slen, &alg->stats.compress.decompress_tlen);
	}
	crypto_alg_put(alg);
}
EXPORT_SYMBOL_GPL(crypto_stats_decompress);

void crypto_stats_ahash_update(unsigned int nbytes, int ret,
			       struct crypto_alg *alg)
{
	if (ret && ret != -EINPROGRESS && ret != -EBUSY)
		atomic64_inc(&alg->stats.hash.err_cnt);
	else
		atomic64_add(nbytes, &alg->stats.hash.hash_tlen);
	crypto_alg_put(alg);
}
EXPORT_SYMBOL_GPL(crypto_stats_ahash_update);

void crypto_stats_ahash_final(unsigned int nbytes, int ret,
			      struct crypto_alg *alg)
{
	if (ret && ret != -EINPROGRESS && ret != -EBUSY) {
		atomic64_inc(&alg->stats.hash.err_cnt);
	} else {
		atomic64_inc(&alg->stats.hash.hash_cnt);
		atomic64_add(nbytes, &alg->stats.hash.hash_tlen);
	}
	crypto_alg_put(alg);
}
EXPORT_SYMBOL_GPL(crypto_stats_ahash_final);

void crypto_stats_kpp_set_secret(struct crypto_alg *alg, int ret)
{
	if (ret)
		atomic64_inc(&alg->stats.kpp.err_cnt);
	else
		atomic64_inc(&alg->stats.kpp.setsecret_cnt);
	crypto_alg_put(alg);
}
EXPORT_SYMBOL_GPL(crypto_stats_kpp_set_secret);

void crypto_stats_kpp_generate_public_key(struct crypto_alg *alg, int ret)
{
	if (ret)
		atomic64_inc(&alg->stats.kpp.err_cnt);
	else
		atomic64_inc(&alg->stats.kpp.generate_public_key_cnt);
	crypto_alg_put(alg);
}
EXPORT_SYMBOL_GPL(crypto_stats_kpp_generate_public_key);

void crypto_stats_kpp_compute_shared_secret(struct crypto_alg *alg, int ret)
{
	if (ret)
		atomic64_inc(&alg->stats.kpp.err_cnt);
	else
		atomic64_inc(&alg->stats.kpp.compute_shared_secret_cnt);
	crypto_alg_put(alg);
}
EXPORT_SYMBOL_GPL(crypto_stats_kpp_compute_shared_secret);

void crypto_stats_rng_seed(struct crypto_alg *alg, int ret)
{
	if (ret && ret != -EINPROGRESS && ret != -EBUSY)
		atomic64_inc(&alg->stats.rng.err_cnt);
	else
		atomic64_inc(&alg->stats.rng.seed_cnt);
	crypto_alg_put(alg);
}
EXPORT_SYMBOL_GPL(crypto_stats_rng_seed);

void crypto_stats_rng_generate(struct crypto_alg *alg, unsigned int dlen,
			       int ret)
{
	if (ret && ret != -EINPROGRESS && ret != -EBUSY) {
		atomic64_inc(&alg->stats.rng.err_cnt);
	} else {
		atomic64_inc(&alg->stats.rng.generate_cnt);
		atomic64_add(dlen, &alg->stats.rng.generate_tlen);
	}
	crypto_alg_put(alg);
}
EXPORT_SYMBOL_GPL(crypto_stats_rng_generate);

void crypto_stats_skcipher_encrypt(unsigned int cryptlen, int ret,
				   struct crypto_alg *alg)
{
	if (ret && ret != -EINPROGRESS && ret != -EBUSY) {
		atomic64_inc(&alg->stats.cipher.err_cnt);
	} else {
		atomic64_inc(&alg->stats.cipher.encrypt_cnt);
		atomic64_add(cryptlen, &alg->stats.cipher.encrypt_tlen);
	}
	crypto_alg_put(alg);
}
EXPORT_SYMBOL_GPL(crypto_stats_skcipher_encrypt);

void crypto_stats_skcipher_decrypt(unsigned int cryptlen, int ret,
				   struct crypto_alg *alg)
{
	if (ret && ret != -EINPROGRESS && ret != -EBUSY) {
		atomic64_inc(&alg->stats.cipher.err_cnt);
	} else {
		atomic64_inc(&alg->stats.cipher.decrypt_cnt);
		atomic64_add(cryptlen, &alg->stats.cipher.decrypt_tlen);
	}
	crypto_alg_put(alg);
}
EXPORT_SYMBOL_GPL(crypto_stats_skcipher_decrypt);
#endif

static int __init crypto_algapi_init(void)
{
	crypto_init_proc();
	return 0;
}

static void __exit crypto_algapi_exit(void)
{
	crypto_exit_proc();
}

module_init(crypto_algapi_init);
module_exit(crypto_algapi_exit);

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Cryptographic algorithms API");
