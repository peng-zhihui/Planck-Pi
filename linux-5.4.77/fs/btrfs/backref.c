// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) 2011 STRATO.  All rights reserved.
 */

#include <linux/mm.h>
#include <linux/rbtree.h>
#include <trace/events/btrfs.h>
#include "ctree.h"
#include "disk-io.h"
#include "backref.h"
#include "ulist.h"
#include "transaction.h"
#include "delayed-ref.h"
#include "locking.h"

/* Just an arbitrary number so we can be sure this happened */
#define BACKREF_FOUND_SHARED 6

struct extent_inode_elem {
	u64 inum;
	u64 offset;
	struct extent_inode_elem *next;
};

static int check_extent_in_eb(const struct btrfs_key *key,
			      const struct extent_buffer *eb,
			      const struct btrfs_file_extent_item *fi,
			      u64 extent_item_pos,
			      struct extent_inode_elem **eie,
			      bool ignore_offset)
{
	u64 offset = 0;
	struct extent_inode_elem *e;

	if (!ignore_offset &&
	    !btrfs_file_extent_compression(eb, fi) &&
	    !btrfs_file_extent_encryption(eb, fi) &&
	    !btrfs_file_extent_other_encoding(eb, fi)) {
		u64 data_offset;
		u64 data_len;

		data_offset = btrfs_file_extent_offset(eb, fi);
		data_len = btrfs_file_extent_num_bytes(eb, fi);

		if (extent_item_pos < data_offset ||
		    extent_item_pos >= data_offset + data_len)
			return 1;
		offset = extent_item_pos - data_offset;
	}

	e = kmalloc(sizeof(*e), GFP_NOFS);
	if (!e)
		return -ENOMEM;

	e->next = *eie;
	e->inum = key->objectid;
	e->offset = key->offset + offset;
	*eie = e;

	return 0;
}

static void free_inode_elem_list(struct extent_inode_elem *eie)
{
	struct extent_inode_elem *eie_next;

	for (; eie; eie = eie_next) {
		eie_next = eie->next;
		kfree(eie);
	}
}

static int find_extent_in_eb(const struct extent_buffer *eb,
			     u64 wanted_disk_byte, u64 extent_item_pos,
			     struct extent_inode_elem **eie,
			     bool ignore_offset)
{
	u64 disk_byte;
	struct btrfs_key key;
	struct btrfs_file_extent_item *fi;
	int slot;
	int nritems;
	int extent_type;
	int ret;

	/*
	 * from the shared data ref, we only have the leaf but we need
	 * the key. thus, we must look into all items and see that we
	 * find one (some) with a reference to our extent item.
	 */
	nritems = btrfs_header_nritems(eb);
	for (slot = 0; slot < nritems; ++slot) {
		btrfs_item_key_to_cpu(eb, &key, slot);
		if (key.type != BTRFS_EXTENT_DATA_KEY)
			continue;
		fi = btrfs_item_ptr(eb, slot, struct btrfs_file_extent_item);
		extent_type = btrfs_file_extent_type(eb, fi);
		if (extent_type == BTRFS_FILE_EXTENT_INLINE)
			continue;
		/* don't skip BTRFS_FILE_EXTENT_PREALLOC, we can handle that */
		disk_byte = btrfs_file_extent_disk_bytenr(eb, fi);
		if (disk_byte != wanted_disk_byte)
			continue;

		ret = check_extent_in_eb(&key, eb, fi, extent_item_pos, eie, ignore_offset);
		if (ret < 0)
			return ret;
	}

	return 0;
}

struct preftree {
	struct rb_root_cached root;
	unsigned int count;
};

#define PREFTREE_INIT	{ .root = RB_ROOT_CACHED, .count = 0 }

struct preftrees {
	struct preftree direct;    /* BTRFS_SHARED_[DATA|BLOCK]_REF_KEY */
	struct preftree indirect;  /* BTRFS_[TREE_BLOCK|EXTENT_DATA]_REF_KEY */
	struct preftree indirect_missing_keys;
};

/*
 * Checks for a shared extent during backref search.
 *
 * The share_count tracks prelim_refs (direct and indirect) having a
 * ref->count >0:
 *  - incremented when a ref->count transitions to >0
 *  - decremented when a ref->count transitions to <1
 */
struct share_check {
	u64 root_objectid;
	u64 inum;
	int share_count;
};

static inline int extent_is_shared(struct share_check *sc)
{
	return (sc && sc->share_count > 1) ? BACKREF_FOUND_SHARED : 0;
}

static struct kmem_cache *btrfs_prelim_ref_cache;

int __init btrfs_prelim_ref_init(void)
{
	btrfs_prelim_ref_cache = kmem_cache_create("btrfs_prelim_ref",
					sizeof(struct prelim_ref),
					0,
					SLAB_MEM_SPREAD,
					NULL);
	if (!btrfs_prelim_ref_cache)
		return -ENOMEM;
	return 0;
}

void __cold btrfs_prelim_ref_exit(void)
{
	kmem_cache_destroy(btrfs_prelim_ref_cache);
}

static void free_pref(struct prelim_ref *ref)
{
	kmem_cache_free(btrfs_prelim_ref_cache, ref);
}

/*
 * Return 0 when both refs are for the same block (and can be merged).
 * A -1 return indicates ref1 is a 'lower' block than ref2, while 1
 * indicates a 'higher' block.
 */
static int prelim_ref_compare(struct prelim_ref *ref1,
			      struct prelim_ref *ref2)
{
	if (ref1->level < ref2->level)
		return -1;
	if (ref1->level > ref2->level)
		return 1;
	if (ref1->root_id < ref2->root_id)
		return -1;
	if (ref1->root_id > ref2->root_id)
		return 1;
	if (ref1->key_for_search.type < ref2->key_for_search.type)
		return -1;
	if (ref1->key_for_search.type > ref2->key_for_search.type)
		return 1;
	if (ref1->key_for_search.objectid < ref2->key_for_search.objectid)
		return -1;
	if (ref1->key_for_search.objectid > ref2->key_for_search.objectid)
		return 1;
	if (ref1->key_for_search.offset < ref2->key_for_search.offset)
		return -1;
	if (ref1->key_for_search.offset > ref2->key_for_search.offset)
		return 1;
	if (ref1->parent < ref2->parent)
		return -1;
	if (ref1->parent > ref2->parent)
		return 1;

	return 0;
}

static void update_share_count(struct share_check *sc, int oldcount,
			       int newcount)
{
	if ((!sc) || (oldcount == 0 && newcount < 1))
		return;

	if (oldcount > 0 && newcount < 1)
		sc->share_count--;
	else if (oldcount < 1 && newcount > 0)
		sc->share_count++;
}

/*
 * Add @newref to the @root rbtree, merging identical refs.
 *
 * Callers should assume that newref has been freed after calling.
 */
static void prelim_ref_insert(const struct btrfs_fs_info *fs_info,
			      struct preftree *preftree,
			      struct prelim_ref *newref,
			      struct share_check *sc)
{
	struct rb_root_cached *root;
	struct rb_node **p;
	struct rb_node *parent = NULL;
	struct prelim_ref *ref;
	int result;
	bool leftmost = true;

	root = &preftree->root;
	p = &root->rb_root.rb_node;

	while (*p) {
		parent = *p;
		ref = rb_entry(parent, struct prelim_ref, rbnode);
		result = prelim_ref_compare(ref, newref);
		if (result < 0) {
			p = &(*p)->rb_left;
		} else if (result > 0) {
			p = &(*p)->rb_right;
			leftmost = false;
		} else {
			/* Identical refs, merge them and free @newref */
			struct extent_inode_elem *eie = ref->inode_list;

			while (eie && eie->next)
				eie = eie->next;

			if (!eie)
				ref->inode_list = newref->inode_list;
			else
				eie->next = newref->inode_list;
			trace_btrfs_prelim_ref_merge(fs_info, ref, newref,
						     preftree->count);
			/*
			 * A delayed ref can have newref->count < 0.
			 * The ref->count is updated to follow any
			 * BTRFS_[ADD|DROP]_DELAYED_REF actions.
			 */
			update_share_count(sc, ref->count,
					   ref->count + newref->count);
			ref->count += newref->count;
			free_pref(newref);
			return;
		}
	}

	update_share_count(sc, 0, newref->count);
	preftree->count++;
	trace_btrfs_prelim_ref_insert(fs_info, newref, NULL, preftree->count);
	rb_link_node(&newref->rbnode, parent, p);
	rb_insert_color_cached(&newref->rbnode, root, leftmost);
}

/*
 * Release the entire tree.  We don't care about internal consistency so
 * just free everything and then reset the tree root.
 */
static void prelim_release(struct preftree *preftree)
{
	struct prelim_ref *ref, *next_ref;

	rbtree_postorder_for_each_entry_safe(ref, next_ref,
					     &preftree->root.rb_root, rbnode)
		free_pref(ref);

	preftree->root = RB_ROOT_CACHED;
	preftree->count = 0;
}

/*
 * the rules for all callers of this function are:
 * - obtaining the parent is the goal
 * - if you add a key, you must know that it is a correct key
 * - if you cannot add the parent or a correct key, then we will look into the
 *   block later to set a correct key
 *
 * delayed refs
 * ============
 *        backref type | shared | indirect | shared | indirect
 * information         |   tree |     tree |   data |     data
 * --------------------+--------+----------+--------+----------
 *      parent logical |    y   |     -    |    -   |     -
 *      key to resolve |    -   |     y    |    y   |     y
 *  tree block logical |    -   |     -    |    -   |     -
 *  root for resolving |    y   |     y    |    y   |     y
 *
 * - column 1:       we've the parent -> done
 * - column 2, 3, 4: we use the key to find the parent
 *
 * on disk refs (inline or keyed)
 * ==============================
 *        backref type | shared | indirect | shared | indirect
 * information         |   tree |     tree |   data |     data
 * --------------------+--------+----------+--------+----------
 *      parent logical |    y   |     -    |    y   |     -
 *      key to resolve |    -   |     -    |    -   |     y
 *  tree block logical |    y   |     y    |    y   |     y
 *  root for resolving |    -   |     y    |    y   |     y
 *
 * - column 1, 3: we've the parent -> done
 * - column 2:    we take the first key from the block to find the parent
 *                (see add_missing_keys)
 * - column 4:    we use the key to find the parent
 *
 * additional information that's available but not required to find the parent
 * block might help in merging entries to gain some speed.
 */
static int add_prelim_ref(const struct btrfs_fs_info *fs_info,
			  struct preftree *preftree, u64 root_id,
			  const struct btrfs_key *key, int level, u64 parent,
			  u64 wanted_disk_byte, int count,
			  struct share_check *sc, gfp_t gfp_mask)
{
	struct prelim_ref *ref;

	if (root_id == BTRFS_DATA_RELOC_TREE_OBJECTID)
		return 0;

	ref = kmem_cache_alloc(btrfs_prelim_ref_cache, gfp_mask);
	if (!ref)
		return -ENOMEM;

	ref->root_id = root_id;
	if (key) {
		ref->key_for_search = *key;
		/*
		 * We can often find data backrefs with an offset that is too
		 * large (>= LLONG_MAX, maximum allowed file offset) due to
		 * underflows when subtracting a file's offset with the data
		 * offset of its corresponding extent data item. This can
		 * happen for example in the clone ioctl.
		 * So if we detect such case we set the search key's offset to
		 * zero to make sure we will find the matching file extent item
		 * at add_all_parents(), otherwise we will miss it because the
		 * offset taken form the backref is much larger then the offset
		 * of the file extent item. This can make us scan a very large
		 * number of file extent items, but at least it will not make
		 * us miss any.
		 * This is an ugly workaround for a behaviour that should have
		 * never existed, but it does and a fix for the clone ioctl
		 * would touch a lot of places, cause backwards incompatibility
		 * and would not fix the problem for extents cloned with older
		 * kernels.
		 */
		if (ref->key_for_search.type == BTRFS_EXTENT_DATA_KEY &&
		    ref->key_for_search.offset >= LLONG_MAX)
			ref->key_for_search.offset = 0;
	} else {
		memset(&ref->key_for_search, 0, sizeof(ref->key_for_search));
	}

	ref->inode_list = NULL;
	ref->level = level;
	ref->count = count;
	ref->parent = parent;
	ref->wanted_disk_byte = wanted_disk_byte;
	prelim_ref_insert(fs_info, preftree, ref, sc);
	return extent_is_shared(sc);
}

/* direct refs use root == 0, key == NULL */
static int add_direct_ref(const struct btrfs_fs_info *fs_info,
			  struct preftrees *preftrees, int level, u64 parent,
			  u64 wanted_disk_byte, int count,
			  struct share_check *sc, gfp_t gfp_mask)
{
	return add_prelim_ref(fs_info, &preftrees->direct, 0, NULL, level,
			      parent, wanted_disk_byte, count, sc, gfp_mask);
}

/* indirect refs use parent == 0 */
static int add_indirect_ref(const struct btrfs_fs_info *fs_info,
			    struct preftrees *preftrees, u64 root_id,
			    const struct btrfs_key *key, int level,
			    u64 wanted_disk_byte, int count,
			    struct share_check *sc, gfp_t gfp_mask)
{
	struct preftree *tree = &preftrees->indirect;

	if (!key)
		tree = &preftrees->indirect_missing_keys;
	return add_prelim_ref(fs_info, tree, root_id, key, level, 0,
			      wanted_disk_byte, count, sc, gfp_mask);
}

static int add_all_parents(struct btrfs_root *root, struct btrfs_path *path,
			   struct ulist *parents, struct prelim_ref *ref,
			   int level, u64 time_seq, const u64 *extent_item_pos,
			   u64 total_refs, bool ignore_offset)
{
	int ret = 0;
	int slot;
	struct extent_buffer *eb;
	struct btrfs_key key;
	struct btrfs_key *key_for_search = &ref->key_for_search;
	struct btrfs_file_extent_item *fi;
	struct extent_inode_elem *eie = NULL, *old = NULL;
	u64 disk_byte;
	u64 wanted_disk_byte = ref->wanted_disk_byte;
	u64 count = 0;

	if (level != 0) {
		eb = path->nodes[level];
		ret = ulist_add(parents, eb->start, 0, GFP_NOFS);
		if (ret < 0)
			return ret;
		return 0;
	}

	/*
	 * We normally enter this function with the path already pointing to
	 * the first item to check. But sometimes, we may enter it with
	 * slot==nritems. In that case, go to the next leaf before we continue.
	 */
	if (path->slots[0] >= btrfs_header_nritems(path->nodes[0])) {
		if (time_seq == SEQ_LAST)
			ret = btrfs_next_leaf(root, path);
		else
			ret = btrfs_next_old_leaf(root, path, time_seq);
	}

	while (!ret && count < total_refs) {
		eb = path->nodes[0];
		slot = path->slots[0];

		btrfs_item_key_to_cpu(eb, &key, slot);

		if (key.objectid != key_for_search->objectid ||
		    key.type != BTRFS_EXTENT_DATA_KEY)
			break;

		fi = btrfs_item_ptr(eb, slot, struct btrfs_file_extent_item);
		disk_byte = btrfs_file_extent_disk_bytenr(eb, fi);

		if (disk_byte == wanted_disk_byte) {
			eie = NULL;
			old = NULL;
			count++;
			if (extent_item_pos) {
				ret = check_extent_in_eb(&key, eb, fi,
						*extent_item_pos,
						&eie, ignore_offset);
				if (ret < 0)
					break;
			}
			if (ret > 0)
				goto next;
			ret = ulist_add_merge_ptr(parents, eb->start,
						  eie, (void **)&old, GFP_NOFS);
			if (ret < 0)
				break;
			if (!ret && extent_item_pos) {
				while (old->next)
					old = old->next;
				old->next = eie;
			}
			eie = NULL;
		}
next:
		if (time_seq == SEQ_LAST)
			ret = btrfs_next_item(root, path);
		else
			ret = btrfs_next_old_item(root, path, time_seq);
	}

	if (ret > 0)
		ret = 0;
	else if (ret < 0)
		free_inode_elem_list(eie);
	return ret;
}

/*
 * resolve an indirect backref in the form (root_id, key, level)
 * to a logical address
 */
static int resolve_indirect_ref(struct btrfs_fs_info *fs_info,
				struct btrfs_path *path, u64 time_seq,
				struct prelim_ref *ref, struct ulist *parents,
				const u64 *extent_item_pos, u64 total_refs,
				bool ignore_offset)
{
	struct btrfs_root *root;
	struct btrfs_key root_key;
	struct extent_buffer *eb;
	int ret = 0;
	int root_level;
	int level = ref->level;
	int index;

	root_key.objectid = ref->root_id;
	root_key.type = BTRFS_ROOT_ITEM_KEY;
	root_key.offset = (u64)-1;

	index = srcu_read_lock(&fs_info->subvol_srcu);

	root = btrfs_get_fs_root(fs_info, &root_key, false);
	if (IS_ERR(root)) {
		srcu_read_unlock(&fs_info->subvol_srcu, index);
		ret = PTR_ERR(root);
		goto out;
	}

	if (btrfs_is_testing(fs_info)) {
		srcu_read_unlock(&fs_info->subvol_srcu, index);
		ret = -ENOENT;
		goto out;
	}

	if (path->search_commit_root)
		root_level = btrfs_header_level(root->commit_root);
	else if (time_seq == SEQ_LAST)
		root_level = btrfs_header_level(root->node);
	else
		root_level = btrfs_old_root_level(root, time_seq);

	if (root_level + 1 == level) {
		srcu_read_unlock(&fs_info->subvol_srcu, index);
		goto out;
	}

	path->lowest_level = level;
	if (time_seq == SEQ_LAST)
		ret = btrfs_search_slot(NULL, root, &ref->key_for_search, path,
					0, 0);
	else
		ret = btrfs_search_old_slot(root, &ref->key_for_search, path,
					    time_seq);

	/* root node has been locked, we can release @subvol_srcu safely here */
	srcu_read_unlock(&fs_info->subvol_srcu, index);

	btrfs_debug(fs_info,
		"search slot in root %llu (level %d, ref count %d) returned %d for key (%llu %u %llu)",
		 ref->root_id, level, ref->count, ret,
		 ref->key_for_search.objectid, ref->key_for_search.type,
		 ref->key_for_search.offset);
	if (ret < 0)
		goto out;

	eb = path->nodes[level];
	while (!eb) {
		if (WARN_ON(!level)) {
			ret = 1;
			goto out;
		}
		level--;
		eb = path->nodes[level];
	}

	ret = add_all_parents(root, path, parents, ref, level, time_seq,
			      extent_item_pos, total_refs, ignore_offset);
out:
	path->lowest_level = 0;
	btrfs_release_path(path);
	return ret;
}

static struct extent_inode_elem *
unode_aux_to_inode_list(struct ulist_node *node)
{
	if (!node)
		return NULL;
	return (struct extent_inode_elem *)(uintptr_t)node->aux;
}

/*
 * We maintain three separate rbtrees: one for direct refs, one for
 * indirect refs which have a key, and one for indirect refs which do not
 * have a key. Each tree does merge on insertion.
 *
 * Once all of the references are located, we iterate over the tree of
 * indirect refs with missing keys. An appropriate key is located and
 * the ref is moved onto the tree for indirect refs. After all missing
 * keys are thus located, we iterate over the indirect ref tree, resolve
 * each reference, and then insert the resolved reference onto the
 * direct tree (merging there too).
 *
 * New backrefs (i.e., for parent nodes) are added to the appropriate
 * rbtree as they are encountered. The new backrefs are subsequently
 * resolved as above.
 */
static int resolve_indirect_refs(struct btrfs_fs_info *fs_info,
				 struct btrfs_path *path, u64 time_seq,
				 struct preftrees *preftrees,
				 const u64 *extent_item_pos, u64 total_refs,
				 struct share_check *sc, bool ignore_offset)
{
	int err;
	int ret = 0;
	struct ulist *parents;
	struct ulist_node *node;
	struct ulist_iterator uiter;
	struct rb_node *rnode;

	parents = ulist_alloc(GFP_NOFS);
	if (!parents)
		return -ENOMEM;

	/*
	 * We could trade memory usage for performance here by iterating
	 * the tree, allocating new refs for each insertion, and then
	 * freeing the entire indirect tree when we're done.  In some test
	 * cases, the tree can grow quite large (~200k objects).
	 */
	while ((rnode = rb_first_cached(&preftrees->indirect.root))) {
		struct prelim_ref *ref;

		ref = rb_entry(rnode, struct prelim_ref, rbnode);
		if (WARN(ref->parent,
			 "BUG: direct ref found in indirect tree")) {
			ret = -EINVAL;
			goto out;
		}

		rb_erase_cached(&ref->rbnode, &preftrees->indirect.root);
		preftrees->indirect.count--;

		if (ref->count == 0) {
			free_pref(ref);
			continue;
		}

		if (sc && sc->root_objectid &&
		    ref->root_id != sc->root_objectid) {
			free_pref(ref);
			ret = BACKREF_FOUND_SHARED;
			goto out;
		}
		err = resolve_indirect_ref(fs_info, path, time_seq, ref,
					   parents, extent_item_pos,
					   total_refs, ignore_offset);
		/*
		 * we can only tolerate ENOENT,otherwise,we should catch error
		 * and return directly.
		 */
		if (err == -ENOENT) {
			prelim_ref_insert(fs_info, &preftrees->direct, ref,
					  NULL);
			continue;
		} else if (err) {
			free_pref(ref);
			ret = err;
			goto out;
		}

		/* we put the first parent into the ref at hand */
		ULIST_ITER_INIT(&uiter);
		node = ulist_next(parents, &uiter);
		ref->parent = node ? node->val : 0;
		ref->inode_list = unode_aux_to_inode_list(node);

		/* Add a prelim_ref(s) for any other parent(s). */
		while ((node = ulist_next(parents, &uiter))) {
			struct prelim_ref *new_ref;

			new_ref = kmem_cache_alloc(btrfs_prelim_ref_cache,
						   GFP_NOFS);
			if (!new_ref) {
				free_pref(ref);
				ret = -ENOMEM;
				goto out;
			}
			memcpy(new_ref, ref, sizeof(*ref));
			new_ref->parent = node->val;
			new_ref->inode_list = unode_aux_to_inode_list(node);
			prelim_ref_insert(fs_info, &preftrees->direct,
					  new_ref, NULL);
		}

		/*
		 * Now it's a direct ref, put it in the direct tree. We must
		 * do this last because the ref could be merged/freed here.
		 */
		prelim_ref_insert(fs_info, &preftrees->direct, ref, NULL);

		ulist_reinit(parents);
		cond_resched();
	}
out:
	ulist_free(parents);
	return ret;
}

/*
 * read tree blocks and add keys where required.
 */
static int add_missing_keys(struct btrfs_fs_info *fs_info,
			    struct preftrees *preftrees, bool lock)
{
	struct prelim_ref *ref;
	struct extent_buffer *eb;
	struct preftree *tree = &preftrees->indirect_missing_keys;
	struct rb_node *node;

	while ((node = rb_first_cached(&tree->root))) {
		ref = rb_entry(node, struct prelim_ref, rbnode);
		rb_erase_cached(node, &tree->root);

		BUG_ON(ref->parent);	/* should not be a direct ref */
		BUG_ON(ref->key_for_search.type);
		BUG_ON(!ref->wanted_disk_byte);

		eb = read_tree_block(fs_info, ref->wanted_disk_byte, 0,
				     ref->level - 1, NULL);
		if (IS_ERR(eb)) {
			free_pref(ref);
			return PTR_ERR(eb);
		} else if (!extent_buffer_uptodate(eb)) {
			free_pref(ref);
			free_extent_buffer(eb);
			return -EIO;
		}
		if (lock)
			btrfs_tree_read_lock(eb);
		if (btrfs_header_level(eb) == 0)
			btrfs_item_key_to_cpu(eb, &ref->key_for_search, 0);
		else
			btrfs_node_key_to_cpu(eb, &ref->key_for_search, 0);
		if (lock)
			btrfs_tree_read_unlock(eb);
		free_extent_buffer(eb);
		prelim_ref_insert(fs_info, &preftrees->indirect, ref, NULL);
		cond_resched();
	}
	return 0;
}

/*
 * add all currently queued delayed refs from this head whose seq nr is
 * smaller or equal that seq to the list
 */
static int add_delayed_refs(const struct btrfs_fs_info *fs_info,
			    struct btrfs_delayed_ref_head *head, u64 seq,
			    struct preftrees *preftrees, u64 *total_refs,
			    struct share_check *sc)
{
	struct btrfs_delayed_ref_node *node;
	struct btrfs_delayed_extent_op *extent_op = head->extent_op;
	struct btrfs_key key;
	struct btrfs_key tmp_op_key;
	struct rb_node *n;
	int count;
	int ret = 0;

	if (extent_op && extent_op->update_key)
		btrfs_disk_key_to_cpu(&tmp_op_key, &extent_op->key);

	spin_lock(&head->lock);
	for (n = rb_first_cached(&head->ref_tree); n; n = rb_next(n)) {
		node = rb_entry(n, struct btrfs_delayed_ref_node,
				ref_node);
		if (node->seq > seq)
			continue;

		switch (node->action) {
		case BTRFS_ADD_DELAYED_EXTENT:
		case BTRFS_UPDATE_DELAYED_HEAD:
			WARN_ON(1);
			continue;
		case BTRFS_ADD_DELAYED_REF:
			count = node->ref_mod;
			break;
		case BTRFS_DROP_DELAYED_REF:
			count = node->ref_mod * -1;
			break;
		default:
			BUG();
		}
		*total_refs += count;
		switch (node->type) {
		case BTRFS_TREE_BLOCK_REF_KEY: {
			/* NORMAL INDIRECT METADATA backref */
			struct btrfs_delayed_tree_ref *ref;

			ref = btrfs_delayed_node_to_tree_ref(node);
			ret = add_indirect_ref(fs_info, preftrees, ref->root,
					       &tmp_op_key, ref->level + 1,
					       node->bytenr, count, sc,
					       GFP_ATOMIC);
			break;
		}
		case BTRFS_SHARED_BLOCK_REF_KEY: {
			/* SHARED DIRECT METADATA backref */
			struct btrfs_delayed_tree_ref *ref;

			ref = btrfs_delayed_node_to_tree_ref(node);

			ret = add_direct_ref(fs_info, preftrees, ref->level + 1,
					     ref->parent, node->bytenr, count,
					     sc, GFP_ATOMIC);
			break;
		}
		case BTRFS_EXTENT_DATA_REF_KEY: {
			/* NORMAL INDIRECT DATA backref */
			struct btrfs_delayed_data_ref *ref;
			ref = btrfs_delayed_node_to_data_ref(node);

			key.objectid = ref->objectid;
			key.type = BTRFS_EXTENT_DATA_KEY;
			key.offset = ref->offset;

			/*
			 * Found a inum that doesn't match our known inum, we
			 * know it's shared.
			 */
			if (sc && sc->inum && ref->objectid != sc->inum) {
				ret = BACKREF_FOUND_SHARED;
				goto out;
			}

			ret = add_indirect_ref(fs_info, preftrees, ref->root,
					       &key, 0, node->bytenr, count, sc,
					       GFP_ATOMIC);
			break;
		}
		case BTRFS_SHARED_DATA_REF_KEY: {
			/* SHARED DIRECT FULL backref */
			struct btrfs_delayed_data_ref *ref;

			ref = btrfs_delayed_node_to_data_ref(node);

			ret = add_direct_ref(fs_info, preftrees, 0, ref->parent,
					     node->bytenr, count, sc,
					     GFP_ATOMIC);
			break;
		}
		default:
			WARN_ON(1);
		}
		/*
		 * We must ignore BACKREF_FOUND_SHARED until all delayed
		 * refs have been checked.
		 */
		if (ret && (ret != BACKREF_FOUND_SHARED))
			break;
	}
	if (!ret)
		ret = extent_is_shared(sc);
out:
	spin_unlock(&head->lock);
	return ret;
}

/*
 * add all inline backrefs for bytenr to the list
 *
 * Returns 0 on success, <0 on error, or BACKREF_FOUND_SHARED.
 */
static int add_inline_refs(const struct btrfs_fs_info *fs_info,
			   struct btrfs_path *path, u64 bytenr,
			   int *info_level, struct preftrees *preftrees,
			   u64 *total_refs, struct share_check *sc)
{
	int ret = 0;
	int slot;
	struct extent_buffer *leaf;
	struct btrfs_key key;
	struct btrfs_key found_key;
	unsigned long ptr;
	unsigned long end;
	struct btrfs_extent_item *ei;
	u64 flags;
	u64 item_size;

	/*
	 * enumerate all inline refs
	 */
	leaf = path->nodes[0];
	slot = path->slots[0];

	item_size = btrfs_item_size_nr(leaf, slot);
	BUG_ON(item_size < sizeof(*ei));

	ei = btrfs_item_ptr(leaf, slot, struct btrfs_extent_item);
	flags = btrfs_extent_flags(leaf, ei);
	*total_refs += btrfs_extent_refs(leaf, ei);
	btrfs_item_key_to_cpu(leaf, &found_key, slot);

	ptr = (unsigned long)(ei + 1);
	end = (unsigned long)ei + item_size;

	if (found_key.type == BTRFS_EXTENT_ITEM_KEY &&
	    flags & BTRFS_EXTENT_FLAG_TREE_BLOCK) {
		struct btrfs_tree_block_info *info;

		info = (struct btrfs_tree_block_info *)ptr;
		*info_level = btrfs_tree_block_level(leaf, info);
		ptr += sizeof(struct btrfs_tree_block_info);
		BUG_ON(ptr > end);
	} else if (found_key.type == BTRFS_METADATA_ITEM_KEY) {
		*info_level = found_key.offset;
	} else {
		BUG_ON(!(flags & BTRFS_EXTENT_FLAG_DATA));
	}

	while (ptr < end) {
		struct btrfs_extent_inline_ref *iref;
		u64 offset;
		int type;

		iref = (struct btrfs_extent_inline_ref *)ptr;
		type = btrfs_get_extent_inline_ref_type(leaf, iref,
							BTRFS_REF_TYPE_ANY);
		if (type == BTRFS_REF_TYPE_INVALID)
			return -EUCLEAN;

		offset = btrfs_extent_inline_ref_offset(leaf, iref);

		switch (type) {
		case BTRFS_SHARED_BLOCK_REF_KEY:
			ret = add_direct_ref(fs_info, preftrees,
					     *info_level + 1, offset,
					     bytenr, 1, NULL, GFP_NOFS);
			break;
		case BTRFS_SHARED_DATA_REF_KEY: {
			struct btrfs_shared_data_ref *sdref;
			int count;

			sdref = (struct btrfs_shared_data_ref *)(iref + 1);
			count = btrfs_shared_data_ref_count(leaf, sdref);

			ret = add_direct_ref(fs_info, preftrees, 0, offset,
					     bytenr, count, sc, GFP_NOFS);
			break;
		}
		case BTRFS_TREE_BLOCK_REF_KEY:
			ret = add_indirect_ref(fs_info, preftrees, offset,
					       NULL, *info_level + 1,
					       bytenr, 1, NULL, GFP_NOFS);
			break;
		case BTRFS_EXTENT_DATA_REF_KEY: {
			struct btrfs_extent_data_ref *dref;
			int count;
			u64 root;

			dref = (struct btrfs_extent_data_ref *)(&iref->offset);
			count = btrfs_extent_data_ref_count(leaf, dref);
			key.objectid = btrfs_extent_data_ref_objectid(leaf,
								      dref);
			key.type = BTRFS_EXTENT_DATA_KEY;
			key.offset = btrfs_extent_data_ref_offset(leaf, dref);

			if (sc && sc->inum && key.objectid != sc->inum) {
				ret = BACKREF_FOUND_SHARED;
				break;
			}

			root = btrfs_extent_data_ref_root(leaf, dref);

			ret = add_indirect_ref(fs_info, preftrees, root,
					       &key, 0, bytenr, count,
					       sc, GFP_NOFS);
			break;
		}
		default:
			WARN_ON(1);
		}
		if (ret)
			return ret;
		ptr += btrfs_extent_inline_ref_size(type);
	}

	return 0;
}

/*
 * add all non-inline backrefs for bytenr to the list
 *
 * Returns 0 on success, <0 on error, or BACKREF_FOUND_SHARED.
 */
static int add_keyed_refs(struct btrfs_fs_info *fs_info,
			  struct btrfs_path *path, u64 bytenr,
			  int info_level, struct preftrees *preftrees,
			  struct share_check *sc)
{
	struct btrfs_root *extent_root = fs_info->extent_root;
	int ret;
	int slot;
	struct extent_buffer *leaf;
	struct btrfs_key key;

	while (1) {
		ret = btrfs_next_item(extent_root, path);
		if (ret < 0)
			break;
		if (ret) {
			ret = 0;
			break;
		}

		slot = path->slots[0];
		leaf = path->nodes[0];
		btrfs_item_key_to_cpu(leaf, &key, slot);

		if (key.objectid != bytenr)
			break;
		if (key.type < BTRFS_TREE_BLOCK_REF_KEY)
			continue;
		if (key.type > BTRFS_SHARED_DATA_REF_KEY)
			break;

		switch (key.type) {
		case BTRFS_SHARED_BLOCK_REF_KEY:
			/* SHARED DIRECT METADATA backref */
			ret = add_direct_ref(fs_info, preftrees,
					     info_level + 1, key.offset,
					     bytenr, 1, NULL, GFP_NOFS);
			break;
		case BTRFS_SHARED_DATA_REF_KEY: {
			/* SHARED DIRECT FULL backref */
			struct btrfs_shared_data_ref *sdref;
			int count;

			sdref = btrfs_item_ptr(leaf, slot,
					      struct btrfs_shared_data_ref);
			count = btrfs_shared_data_ref_count(leaf, sdref);
			ret = add_direct_ref(fs_info, preftrees, 0,
					     key.offset, bytenr, count,
					     sc, GFP_NOFS);
			break;
		}
		case BTRFS_TREE_BLOCK_REF_KEY:
			/* NORMAL INDIRECT METADATA backref */
			ret = add_indirect_ref(fs_info, preftrees, key.offset,
					       NULL, info_level + 1, bytenr,
					       1, NULL, GFP_NOFS);
			break;
		case BTRFS_EXTENT_DATA_REF_KEY: {
			/* NORMAL INDIRECT DATA backref */
			struct btrfs_extent_data_ref *dref;
			int count;
			u64 root;

			dref = btrfs_item_ptr(leaf, slot,
					      struct btrfs_extent_data_ref);
			count = btrfs_extent_data_ref_count(leaf, dref);
			key.objectid = btrfs_extent_data_ref_objectid(leaf,
								      dref);
			key.type = BTRFS_EXTENT_DATA_KEY;
			key.offset = btrfs_extent_data_ref_offset(leaf, dref);

			if (sc && sc->inum && key.objectid != sc->inum) {
				ret = BACKREF_FOUND_SHARED;
				break;
			}

			root = btrfs_extent_data_ref_root(leaf, dref);
			ret = add_indirect_ref(fs_info, preftrees, root,
					       &key, 0, bytenr, count,
					       sc, GFP_NOFS);
			break;
		}
		default:
			WARN_ON(1);
		}
		if (ret)
			return ret;

	}

	return ret;
}

/*
 * this adds all existing backrefs (inline backrefs, backrefs and delayed
 * refs) for the given bytenr to the refs list, merges duplicates and resolves
 * indirect refs to their parent bytenr.
 * When roots are found, they're added to the roots list
 *
 * If time_seq is set to SEQ_LAST, it will not search delayed_refs, and behave
 * much like trans == NULL case, the difference only lies in it will not
 * commit root.
 * The special case is for qgroup to search roots in commit_transaction().
 *
 * @sc - if !NULL, then immediately return BACKREF_FOUND_SHARED when a
 * shared extent is detected.
 *
 * Otherwise this returns 0 for success and <0 for an error.
 *
 * If ignore_offset is set to false, only extent refs whose offsets match
 * extent_item_pos are returned.  If true, every extent ref is returned
 * and extent_item_pos is ignored.
 *
 * FIXME some caching might speed things up
 */
static int find_parent_nodes(struct btrfs_trans_handle *trans,
			     struct btrfs_fs_info *fs_info, u64 bytenr,
			     u64 time_seq, struct ulist *refs,
			     struct ulist *roots, const u64 *extent_item_pos,
			     struct share_check *sc, bool ignore_offset)
{
	struct btrfs_key key;
	struct btrfs_path *path;
	struct btrfs_delayed_ref_root *delayed_refs = NULL;
	struct btrfs_delayed_ref_head *head;
	int info_level = 0;
	int ret;
	struct prelim_ref *ref;
	struct rb_node *node;
	struct extent_inode_elem *eie = NULL;
	/* total of both direct AND indirect refs! */
	u64 total_refs = 0;
	struct preftrees preftrees = {
		.direct = PREFTREE_INIT,
		.indirect = PREFTREE_INIT,
		.indirect_missing_keys = PREFTREE_INIT
	};

	key.objectid = bytenr;
	key.offset = (u64)-1;
	if (btrfs_fs_incompat(fs_info, SKINNY_METADATA))
		key.type = BTRFS_METADATA_ITEM_KEY;
	else
		key.type = BTRFS_EXTENT_ITEM_KEY;

	path = btrfs_alloc_path();
	if (!path)
		return -ENOMEM;
	if (!trans) {
		path->search_commit_root = 1;
		path->skip_locking = 1;
	}

	if (time_seq == SEQ_LAST)
		path->skip_locking = 1;

	/*
	 * grab both a lock on the path and a lock on the delayed ref head.
	 * We need both to get a consistent picture of how the refs look
	 * at a specified point in time
	 */
again:
	head = NULL;

	ret = btrfs_search_slot(trans, fs_info->extent_root, &key, path, 0, 0);
	if (ret < 0)
		goto out;
	BUG_ON(ret == 0);

#ifdef CONFIG_BTRFS_FS_RUN_SANITY_TESTS
	if (trans && likely(trans->type != __TRANS_DUMMY) &&
	    time_seq != SEQ_LAST) {
#else
	if (trans && time_seq != SEQ_LAST) {
#endif
		/*
		 * look if there are updates for this ref queued and lock the
		 * head
		 */
		delayed_refs = &trans->transaction->delayed_refs;
		spin_lock(&delayed_refs->lock);
		head = btrfs_find_delayed_ref_head(delayed_refs, bytenr);
		if (head) {
			if (!mutex_trylock(&head->mutex)) {
				refcount_inc(&head->refs);
				spin_unlock(&delayed_refs->lock);

				btrfs_release_path(path);

				/*
				 * Mutex was contended, block until it's
				 * released and try again
				 */
				mutex_lock(&head->mutex);
				mutex_unlock(&head->mutex);
				btrfs_put_delayed_ref_head(head);
				goto again;
			}
			spin_unlock(&delayed_refs->lock);
			ret = add_delayed_refs(fs_info, head, time_seq,
					       &preftrees, &total_refs, sc);
			mutex_unlock(&head->mutex);
			if (ret)
				goto out;
		} else {
			spin_unlock(&delayed_refs->lock);
		}
	}

	if (path->slots[0]) {
		struct extent_buffer *leaf;
		int slot;

		path->slots[0]--;
		leaf = path->nodes[0];
		slot = path->slots[0];
		btrfs_item_key_to_cpu(leaf, &key, slot);
		if (key.objectid == bytenr &&
		    (key.type == BTRFS_EXTENT_ITEM_KEY ||
		     key.type == BTRFS_METADATA_ITEM_KEY)) {
			ret = add_inline_refs(fs_info, path, bytenr,
					      &info_level, &preftrees,
					      &total_refs, sc);
			if (ret)
				goto out;
			ret = add_keyed_refs(fs_info, path, bytenr, info_level,
					     &preftrees, sc);
			if (ret)
				goto out;
		}
	}

	btrfs_release_path(path);

	ret = add_missing_keys(fs_info, &preftrees, path->skip_locking == 0);
	if (ret)
		goto out;

	WARN_ON(!RB_EMPTY_ROOT(&preftrees.indirect_missing_keys.root.rb_root));

	ret = resolve_indirect_refs(fs_info, path, time_seq, &preftrees,
				    extent_item_pos, total_refs, sc, ignore_offset);
	if (ret)
		goto out;

	WARN_ON(!RB_EMPTY_ROOT(&preftrees.indirect.root.rb_root));

	/*
	 * This walks the tree of merged and resolved refs. Tree blocks are
	 * read in as needed. Unique entries are added to the ulist, and
	 * the list of found roots is updated.
	 *
	 * We release the entire tree in one go before returning.
	 */
	node = rb_first_cached(&preftrees.direct.root);
	while (node) {
		ref = rb_entry(node, struct prelim_ref, rbnode);
		node = rb_next(&ref->rbnode);
		/*
		 * ref->count < 0 can happen here if there are delayed
		 * refs with a node->action of BTRFS_DROP_DELAYED_REF.
		 * prelim_ref_insert() relies on this when merging
		 * identical refs to keep the overall count correct.
		 * prelim_ref_insert() will merge only those refs
		 * which compare identically.  Any refs having
		 * e.g. different offsets would not be merged,
		 * and would retain their original ref->count < 0.
		 */
		if (roots && ref->count && ref->root_id && ref->parent == 0) {
			if (sc && sc->root_objectid &&
			    ref->root_id != sc->root_objectid) {
				ret = BACKREF_FOUND_SHARED;
				goto out;
			}

			/* no parent == root of tree */
			ret = ulist_add(roots, ref->root_id, 0, GFP_NOFS);
			if (ret < 0)
				goto out;
		}
		if (ref->count && ref->parent) {
			if (extent_item_pos && !ref->inode_list &&
			    ref->level == 0) {
				struct extent_buffer *eb;

				eb = read_tree_block(fs_info, ref->parent, 0,
						     ref->level, NULL);
				if (IS_ERR(eb)) {
					ret = PTR_ERR(eb);
					goto out;
				} else if (!extent_buffer_uptodate(eb)) {
					free_extent_buffer(eb);
					ret = -EIO;
					goto out;
				}

				if (!path->skip_locking) {
					btrfs_tree_read_lock(eb);
					btrfs_set_lock_blocking_read(eb);
				}
				ret = find_extent_in_eb(eb, bytenr,
							*extent_item_pos, &eie, ignore_offset);
				if (!path->skip_locking)
					btrfs_tree_read_unlock_blocking(eb);
				free_extent_buffer(eb);
				if (ret < 0)
					goto out;
				ref->inode_list = eie;
			}
			ret = ulist_add_merge_ptr(refs, ref->parent,
						  ref->inode_list,
						  (void **)&eie, GFP_NOFS);
			if (ret < 0)
				goto out;
			if (!ret && extent_item_pos) {
				/*
				 * we've recorded that parent, so we must extend
				 * its inode list here
				 */
				BUG_ON(!eie);
				while (eie->next)
					eie = eie->next;
				eie->next = ref->inode_list;
			}
			eie = NULL;
		}
		cond_resched();
	}

out:
	btrfs_free_path(path);

	prelim_release(&preftrees.direct);
	prelim_release(&preftrees.indirect);
	prelim_release(&preftrees.indirect_missing_keys);

	if (ret < 0)
		free_inode_elem_list(eie);
	return ret;
}

static void free_leaf_list(struct ulist *blocks)
{
	struct ulist_node *node = NULL;
	struct extent_inode_elem *eie;
	struct ulist_iterator uiter;

	ULIST_ITER_INIT(&uiter);
	while ((node = ulist_next(blocks, &uiter))) {
		if (!node->aux)
			continue;
		eie = unode_aux_to_inode_list(node);
		free_inode_elem_list(eie);
		node->aux = 0;
	}

	ulist_free(blocks);
}

/*
 * Finds all leafs with a reference to the specified combination of bytenr and
 * offset. key_list_head will point to a list of corresponding keys (caller must
 * free each list element). The leafs will be stored in the leafs ulist, which
 * must be freed with ulist_free.
 *
 * returns 0 on success, <0 on error
 */
static int btrfs_find_all_leafs(struct btrfs_trans_handle *trans,
				struct btrfs_fs_info *fs_info, u64 bytenr,
				u64 time_seq, struct ulist **leafs,
				const u64 *extent_item_pos, bool ignore_offset)
{
	int ret;

	*leafs = ulist_alloc(GFP_NOFS);
	if (!*leafs)
		return -ENOMEM;

	ret = find_parent_nodes(trans, fs_info, bytenr, time_seq,
				*leafs, NULL, extent_item_pos, NULL, ignore_offset);
	if (ret < 0 && ret != -ENOENT) {
		free_leaf_list(*leafs);
		return ret;
	}

	return 0;
}

/*
 * walk all backrefs for a given extent to find all roots that reference this
 * extent. Walking a backref means finding all extents that reference this
 * extent and in turn walk the backrefs of those, too. Naturally this is a
 * recursive process, but here it is implemented in an iterative fashion: We
 * find all referencing extents for the extent in question and put them on a
 * list. In turn, we find all referencing extents for those, further appending
 * to the list. The way we iterate the list allows adding more elements after
 * the current while iterating. The process stops when we reach the end of the
 * list. Found roots are added to the roots list.
 *
 * returns 0 on success, < 0 on error.
 */
static int btrfs_find_all_roots_safe(struct btrfs_trans_handle *trans,
				     struct btrfs_fs_info *fs_info, u64 bytenr,
				     u64 time_seq, struct ulist **roots,
				     bool ignore_offset)
{
	struct ulist *tmp;
	struct ulist_node *node = NULL;
	struct ulist_iterator uiter;
	int ret;

	tmp = ulist_alloc(GFP_NOFS);
	if (!tmp)
		return -ENOMEM;
	*roots = ulist_alloc(GFP_NOFS);
	if (!*roots) {
		ulist_free(tmp);
		return -ENOMEM;
	}

	ULIST_ITER_INIT(&uiter);
	while (1) {
		ret = find_parent_nodes(trans, fs_info, bytenr, time_seq,
					tmp, *roots, NULL, NULL, ignore_offset);
		if (ret < 0 && ret != -ENOENT) {
			ulist_free(tmp);
			ulist_free(*roots);
			*roots = NULL;
			return ret;
		}
		node = ulist_next(tmp, &uiter);
		if (!node)
			break;
		bytenr = node->val;
		cond_resched();
	}

	ulist_free(tmp);
	return 0;
}

int btrfs_find_all_roots(struct btrfs_trans_handle *trans,
			 struct btrfs_fs_info *fs_info, u64 bytenr,
			 u64 time_seq, struct ulist **roots,
			 bool ignore_offset)
{
	int ret;

	if (!trans)
		down_read(&fs_info->commit_root_sem);
	ret = btrfs_find_all_roots_safe(trans, fs_info, bytenr,
					time_seq, roots, ignore_offset);
	if (!trans)
		up_read(&fs_info->commit_root_sem);
	return ret;
}

/**
 * btrfs_check_shared - tell us whether an extent is shared
 *
 * btrfs_check_shared uses the backref walking code but will short
 * circuit as soon as it finds a root or inode that doesn't match the
 * one passed in. This provides a significant performance benefit for
 * callers (such as fiemap) which want to know whether the extent is
 * shared but do not need a ref count.
 *
 * This attempts to attach to the running transaction in order to account for
 * delayed refs, but continues on even when no running transaction exists.
 *
 * Return: 0 if extent is not shared, 1 if it is shared, < 0 on error.
 */
int btrfs_check_shared(struct btrfs_root *root, u64 inum, u64 bytenr,
		struct ulist *roots, struct ulist *tmp)
{
	struct btrfs_fs_info *fs_info = root->fs_info;
	struct btrfs_trans_handle *trans;
	struct ulist_iterator uiter;
	struct ulist_node *node;
	struct seq_list elem = SEQ_LIST_INIT(elem);
	int ret = 0;
	struct share_check shared = {
		.root_objectid = root->root_key.objectid,
		.inum = inum,
		.share_count = 0,
	};

	ulist_init(roots);
	ulist_init(tmp);

	trans = btrfs_join_transaction_nostart(root);
	if (IS_ERR(trans)) {
		if (PTR_ERR(trans) != -ENOENT && PTR_ERR(trans) != -EROFS) {
			ret = PTR_ERR(trans);
			goto out;
		}
		trans = NULL;
		down_read(&fs_info->commit_root_sem);
	} else {
		btrfs_get_tree_mod_seq(fs_info, &elem);
	}

	ULIST_ITER_INIT(&uiter);
	while (1) {
		ret = find_parent_nodes(trans, fs_info, bytenr, elem.seq, tmp,
					roots, NULL, &shared, false);
		if (ret == BACKREF_FOUND_SHARED) {
			/* this is the only condition under which we return 1 */
			ret = 1;
			break;
		}
		if (ret < 0 && ret != -ENOENT)
			break;
		ret = 0;
		node = ulist_next(tmp, &uiter);
		if (!node)
			break;
		bytenr = node->val;
		shared.share_count = 0;
		cond_resched();
	}

	if (trans) {
		btrfs_put_tree_mod_seq(fs_info, &elem);
		btrfs_end_transaction(trans);
	} else {
		up_read(&fs_info->commit_root_sem);
	}
out:
	ulist_release(roots);
	ulist_release(tmp);
	return ret;
}

int btrfs_find_one_extref(struct btrfs_root *root, u64 inode_objectid,
			  u64 start_off, struct btrfs_path *path,
			  struct btrfs_inode_extref **ret_extref,
			  u64 *found_off)
{
	int ret, slot;
	struct btrfs_key key;
	struct btrfs_key found_key;
	struct btrfs_inode_extref *extref;
	const struct extent_buffer *leaf;
	unsigned long ptr;

	key.objectid = inode_objectid;
	key.type = BTRFS_INODE_EXTREF_KEY;
	key.offset = start_off;

	ret = btrfs_search_slot(NULL, root, &key, path, 0, 0);
	if (ret < 0)
		return ret;

	while (1) {
		leaf = path->nodes[0];
		slot = path->slots[0];
		if (slot >= btrfs_header_nritems(leaf)) {
			/*
			 * If the item at offset is not found,
			 * btrfs_search_slot will point us to the slot
			 * where it should be inserted. In our case
			 * that will be the slot directly before the
			 * next INODE_REF_KEY_V2 item. In the case
			 * that we're pointing to the last slot in a
			 * leaf, we must move one leaf over.
			 */
			ret = btrfs_next_leaf(root, path);
			if (ret) {
				if (ret >= 1)
					ret = -ENOENT;
				break;
			}
			continue;
		}

		btrfs_item_key_to_cpu(leaf, &found_key, slot);

		/*
		 * Check that we're still looking at an extended ref key for
		 * this particular objectid. If we have different
		 * objectid or type then there are no more to be found
		 * in the tree and we can exit.
		 */
		ret = -ENOENT;
		if (found_key.objectid != inode_objectid)
			break;
		if (found_key.type != BTRFS_INODE_EXTREF_KEY)
			break;

		ret = 0;
		ptr = btrfs_item_ptr_offset(leaf, path->slots[0]);
		extref = (struct btrfs_inode_extref *)ptr;
		*ret_extref = extref;
		if (found_off)
			*found_off = found_key.offset;
		break;
	}

	return ret;
}

/*
 * this iterates to turn a name (from iref/extref) into a full filesystem path.
 * Elements of the path are separated by '/' and the path is guaranteed to be
 * 0-terminated. the path is only given within the current file system.
 * Therefore, it never starts with a '/'. the caller is responsible to provide
 * "size" bytes in "dest". the dest buffer will be filled backwards. finally,
 * the start point of the resulting string is returned. this pointer is within
 * dest, normally.
 * in case the path buffer would overflow, the pointer is decremented further
 * as if output was written to the buffer, though no more output is actually
 * generated. that way, the caller can determine how much space would be
 * required for the path to fit into the buffer. in that case, the returned
 * value will be smaller than dest. callers must check this!
 */
char *btrfs_ref_to_path(struct btrfs_root *fs_root, struct btrfs_path *path,
			u32 name_len, unsigned long name_off,
			struct extent_buffer *eb_in, u64 parent,
			char *dest, u32 size)
{
	int slot;
	u64 next_inum;
	int ret;
	s64 bytes_left = ((s64)size) - 1;
	struct extent_buffer *eb = eb_in;
	struct btrfs_key found_key;
	int leave_spinning = path->leave_spinning;
	struct btrfs_inode_ref *iref;

	if (bytes_left >= 0)
		dest[bytes_left] = '\0';

	path->leave_spinning = 1;
	while (1) {
		bytes_left -= name_len;
		if (bytes_left >= 0)
			read_extent_buffer(eb, dest + bytes_left,
					   name_off, name_len);
		if (eb != eb_in) {
			if (!path->skip_locking)
				btrfs_tree_read_unlock_blocking(eb);
			free_extent_buffer(eb);
		}
		ret = btrfs_find_item(fs_root, path, parent, 0,
				BTRFS_INODE_REF_KEY, &found_key);
		if (ret > 0)
			ret = -ENOENT;
		if (ret)
			break;

		next_inum = found_key.offset;

		/* regular exit ahead */
		if (parent == next_inum)
			break;

		slot = path->slots[0];
		eb = path->nodes[0];
		/* make sure we can use eb after releasing the path */
		if (eb != eb_in) {
			if (!path->skip_locking)
				btrfs_set_lock_blocking_read(eb);
			path->nodes[0] = NULL;
			path->locks[0] = 0;
		}
		btrfs_release_path(path);
		iref = btrfs_item_ptr(eb, slot, struct btrfs_inode_ref);

		name_len = btrfs_inode_ref_name_len(eb, iref);
		name_off = (unsigned long)(iref + 1);

		parent = next_inum;
		--bytes_left;
		if (bytes_left >= 0)
			dest[bytes_left] = '/';
	}

	btrfs_release_path(path);
	path->leave_spinning = leave_spinning;

	if (ret)
		return ERR_PTR(ret);

	return dest + bytes_left;
}

/*
 * this makes the path point to (logical EXTENT_ITEM *)
 * returns BTRFS_EXTENT_FLAG_DATA for data, BTRFS_EXTENT_FLAG_TREE_BLOCK for
 * tree blocks and <0 on error.
 */
int extent_from_logical(struct btrfs_fs_info *fs_info, u64 logical,
			struct btrfs_path *path, struct btrfs_key *found_key,
			u64 *flags_ret)
{
	int ret;
	u64 flags;
	u64 size = 0;
	u32 item_size;
	const struct extent_buffer *eb;
	struct btrfs_extent_item *ei;
	struct btrfs_key key;

	if (btrfs_fs_incompat(fs_info, SKINNY_METADATA))
		key.type = BTRFS_METADATA_ITEM_KEY;
	else
		key.type = BTRFS_EXTENT_ITEM_KEY;
	key.objectid = logical;
	key.offset = (u64)-1;

	ret = btrfs_search_slot(NULL, fs_info->extent_root, &key, path, 0, 0);
	if (ret < 0)
		return ret;

	ret = btrfs_previous_extent_item(fs_info->extent_root, path, 0);
	if (ret) {
		if (ret > 0)
			ret = -ENOENT;
		return ret;
	}
	btrfs_item_key_to_cpu(path->nodes[0], found_key, path->slots[0]);
	if (found_key->type == BTRFS_METADATA_ITEM_KEY)
		size = fs_info->nodesize;
	else if (found_key->type == BTRFS_EXTENT_ITEM_KEY)
		size = found_key->offset;

	if (found_key->objectid > logical ||
	    found_key->objectid + size <= logical) {
		btrfs_debug(fs_info,
			"logical %llu is not within any extent", logical);
		return -ENOENT;
	}

	eb = path->nodes[0];
	item_size = btrfs_item_size_nr(eb, path->slots[0]);
	BUG_ON(item_size < sizeof(*ei));

	ei = btrfs_item_ptr(eb, path->slots[0], struct btrfs_extent_item);
	flags = btrfs_extent_flags(eb, ei);

	btrfs_debug(fs_info,
		"logical %llu is at position %llu within the extent (%llu EXTENT_ITEM %llu) flags %#llx size %u",
		 logical, logical - found_key->objectid, found_key->objectid,
		 found_key->offset, flags, item_size);

	WARN_ON(!flags_ret);
	if (flags_ret) {
		if (flags & BTRFS_EXTENT_FLAG_TREE_BLOCK)
			*flags_ret = BTRFS_EXTENT_FLAG_TREE_BLOCK;
		else if (flags & BTRFS_EXTENT_FLAG_DATA)
			*flags_ret = BTRFS_EXTENT_FLAG_DATA;
		else
			BUG();
		return 0;
	}

	return -EIO;
}

/*
 * helper function to iterate extent inline refs. ptr must point to a 0 value
 * for the first call and may be modified. it is used to track state.
 * if more refs exist, 0 is returned and the next call to
 * get_extent_inline_ref must pass the modified ptr parameter to get the
 * next ref. after the last ref was processed, 1 is returned.
 * returns <0 on error
 */
static int get_extent_inline_ref(unsigned long *ptr,
				 const struct extent_buffer *eb,
				 const struct btrfs_key *key,
				 const struct btrfs_extent_item *ei,
				 u32 item_size,
				 struct btrfs_extent_inline_ref **out_eiref,
				 int *out_type)
{
	unsigned long end;
	u64 flags;
	struct btrfs_tree_block_info *info;

	if (!*ptr) {
		/* first call */
		flags = btrfs_extent_flags(eb, ei);
		if (flags & BTRFS_EXTENT_FLAG_TREE_BLOCK) {
			if (key->type == BTRFS_METADATA_ITEM_KEY) {
				/* a skinny metadata extent */
				*out_eiref =
				     (struct btrfs_extent_inline_ref *)(ei + 1);
			} else {
				WARN_ON(key->type != BTRFS_EXTENT_ITEM_KEY);
				info = (struct btrfs_tree_block_info *)(ei + 1);
				*out_eiref =
				   (struct btrfs_extent_inline_ref *)(info + 1);
			}
		} else {
			*out_eiref = (struct btrfs_extent_inline_ref *)(ei + 1);
		}
		*ptr = (unsigned long)*out_eiref;
		if ((unsigned long)(*ptr) >= (unsigned long)ei + item_size)
			return -ENOENT;
	}

	end = (unsigned long)ei + item_size;
	*out_eiref = (struct btrfs_extent_inline_ref *)(*ptr);
	*out_type = btrfs_get_extent_inline_ref_type(eb, *out_eiref,
						     BTRFS_REF_TYPE_ANY);
	if (*out_type == BTRFS_REF_TYPE_INVALID)
		return -EUCLEAN;

	*ptr += btrfs_extent_inline_ref_size(*out_type);
	WARN_ON(*ptr > end);
	if (*ptr == end)
		return 1; /* last */

	return 0;
}

/*
 * reads the tree block backref for an extent. tree level and root are returned
 * through out_level and out_root. ptr must point to a 0 value for the first
 * call and may be modified (see get_extent_inline_ref comment).
 * returns 0 if data was provided, 1 if there was no more data to provide or
 * <0 on error.
 */
int tree_backref_for_extent(unsigned long *ptr, struct extent_buffer *eb,
			    struct btrfs_key *key, struct btrfs_extent_item *ei,
			    u32 item_size, u64 *out_root, u8 *out_level)
{
	int ret;
	int type;
	struct btrfs_extent_inline_ref *eiref;

	if (*ptr == (unsigned long)-1)
		return 1;

	while (1) {
		ret = get_extent_inline_ref(ptr, eb, key, ei, item_size,
					      &eiref, &type);
		if (ret < 0)
			return ret;

		if (type == BTRFS_TREE_BLOCK_REF_KEY ||
		    type == BTRFS_SHARED_BLOCK_REF_KEY)
			break;

		if (ret == 1)
			return 1;
	}

	/* we can treat both ref types equally here */
	*out_root = btrfs_extent_inline_ref_offset(eb, eiref);

	if (key->type == BTRFS_EXTENT_ITEM_KEY) {
		struct btrfs_tree_block_info *info;

		info = (struct btrfs_tree_block_info *)(ei + 1);
		*out_level = btrfs_tree_block_level(eb, info);
	} else {
		ASSERT(key->type == BTRFS_METADATA_ITEM_KEY);
		*out_level = (u8)key->offset;
	}

	if (ret == 1)
		*ptr = (unsigned long)-1;

	return 0;
}

static int iterate_leaf_refs(struct btrfs_fs_info *fs_info,
			     struct extent_inode_elem *inode_list,
			     u64 root, u64 extent_item_objectid,
			     iterate_extent_inodes_t *iterate, void *ctx)
{
	struct extent_inode_elem *eie;
	int ret = 0;

	for (eie = inode_list; eie; eie = eie->next) {
		btrfs_debug(fs_info,
			    "ref for %llu resolved, key (%llu EXTEND_DATA %llu), root %llu",
			    extent_item_objectid, eie->inum,
			    eie->offset, root);
		ret = iterate(eie->inum, eie->offset, root, ctx);
		if (ret) {
			btrfs_debug(fs_info,
				    "stopping iteration for %llu due to ret=%d",
				    extent_item_objectid, ret);
			break;
		}
	}

	return ret;
}

/*
 * calls iterate() for every inode that references the extent identified by
 * the given parameters.
 * when the iterator function returns a non-zero value, iteration stops.
 */
int iterate_extent_inodes(struct btrfs_fs_info *fs_info,
				u64 extent_item_objectid, u64 extent_item_pos,
				int search_commit_root,
				iterate_extent_inodes_t *iterate, void *ctx,
				bool ignore_offset)
{
	int ret;
	struct btrfs_trans_handle *trans = NULL;
	struct ulist *refs = NULL;
	struct ulist *roots = NULL;
	struct ulist_node *ref_node = NULL;
	struct ulist_node *root_node = NULL;
	struct seq_list tree_mod_seq_elem = SEQ_LIST_INIT(tree_mod_seq_elem);
	struct ulist_iterator ref_uiter;
	struct ulist_iterator root_uiter;

	btrfs_debug(fs_info, "resolving all inodes for extent %llu",
			extent_item_objectid);

	if (!search_commit_root) {
		trans = btrfs_attach_transaction(fs_info->extent_root);
		if (IS_ERR(trans)) {
			if (PTR_ERR(trans) != -ENOENT &&
			    PTR_ERR(trans) != -EROFS)
				return PTR_ERR(trans);
			trans = NULL;
		}
	}

	if (trans)
		btrfs_get_tree_mod_seq(fs_info, &tree_mod_seq_elem);
	else
		down_read(&fs_info->commit_root_sem);

	ret = btrfs_find_all_leafs(trans, fs_info, extent_item_objectid,
				   tree_mod_seq_elem.seq, &refs,
				   &extent_item_pos, ignore_offset);
	if (ret)
		goto out;

	ULIST_ITER_INIT(&ref_uiter);
	while (!ret && (ref_node = ulist_next(refs, &ref_uiter))) {
		ret = btrfs_find_all_roots_safe(trans, fs_info, ref_node->val,
						tree_mod_seq_elem.seq, &roots,
						ignore_offset);
		if (ret)
			break;
		ULIST_ITER_INIT(&root_uiter);
		while (!ret && (root_node = ulist_next(roots, &root_uiter))) {
			btrfs_debug(fs_info,
				    "root %llu references leaf %llu, data list %#llx",
				    root_node->val, ref_node->val,
				    ref_node->aux);
			ret = iterate_leaf_refs(fs_info,
						(struct extent_inode_elem *)
						(uintptr_t)ref_node->aux,
						root_node->val,
						extent_item_objectid,
						iterate, ctx);
		}
		ulist_free(roots);
	}

	free_leaf_list(refs);
out:
	if (trans) {
		btrfs_put_tree_mod_seq(fs_info, &tree_mod_seq_elem);
		btrfs_end_transaction(trans);
	} else {
		up_read(&fs_info->commit_root_sem);
	}

	return ret;
}

int iterate_inodes_from_logical(u64 logical, struct btrfs_fs_info *fs_info,
				struct btrfs_path *path,
				iterate_extent_inodes_t *iterate, void *ctx,
				bool ignore_offset)
{
	int ret;
	u64 extent_item_pos;
	u64 flags = 0;
	struct btrfs_key found_key;
	int search_commit_root = path->search_commit_root;

	ret = extent_from_logical(fs_info, logical, path, &found_key, &flags);
	btrfs_release_path(path);
	if (ret < 0)
		return ret;
	if (flags & BTRFS_EXTENT_FLAG_TREE_BLOCK)
		return -EINVAL;

	extent_item_pos = logical - found_key.objectid;
	ret = iterate_extent_inodes(fs_info, found_key.objectid,
					extent_item_pos, search_commit_root,
					iterate, ctx, ignore_offset);

	return ret;
}

typedef int (iterate_irefs_t)(u64 parent, u32 name_len, unsigned long name_off,
			      struct extent_buffer *eb, void *ctx);

static int iterate_inode_refs(u64 inum, struct btrfs_root *fs_root,
			      struct btrfs_path *path,
			      iterate_irefs_t *iterate, void *ctx)
{
	int ret = 0;
	int slot;
	u32 cur;
	u32 len;
	u32 name_len;
	u64 parent = 0;
	int found = 0;
	struct extent_buffer *eb;
	struct btrfs_item *item;
	struct btrfs_inode_ref *iref;
	struct btrfs_key found_key;

	while (!ret) {
		ret = btrfs_find_item(fs_root, path, inum,
				parent ? parent + 1 : 0, BTRFS_INODE_REF_KEY,
				&found_key);

		if (ret < 0)
			break;
		if (ret) {
			ret = found ? 0 : -ENOENT;
			break;
		}
		++found;

		parent = found_key.offset;
		slot = path->slots[0];
		eb = btrfs_clone_extent_buffer(path->nodes[0]);
		if (!eb) {
			ret = -ENOMEM;
			break;
		}
		btrfs_release_path(path);

		item = btrfs_item_nr(slot);
		iref = btrfs_item_ptr(eb, slot, struct btrfs_inode_ref);

		for (cur = 0; cur < btrfs_item_size(eb, item); cur += len) {
			name_len = btrfs_inode_ref_name_len(eb, iref);
			/* path must be released before calling iterate()! */
			btrfs_debug(fs_root->fs_info,
				"following ref at offset %u for inode %llu in tree %llu",
				cur, found_key.objectid,
				fs_root->root_key.objectid);
			ret = iterate(parent, name_len,
				      (unsigned long)(iref + 1), eb, ctx);
			if (ret)
				break;
			len = sizeof(*iref) + name_len;
			iref = (struct btrfs_inode_ref *)((char *)iref + len);
		}
		free_extent_buffer(eb);
	}

	btrfs_release_path(path);

	return ret;
}

static int iterate_inode_extrefs(u64 inum, struct btrfs_root *fs_root,
				 struct btrfs_path *path,
				 iterate_irefs_t *iterate, void *ctx)
{
	int ret;
	int slot;
	u64 offset = 0;
	u64 parent;
	int found = 0;
	struct extent_buffer *eb;
	struct btrfs_inode_extref *extref;
	u32 item_size;
	u32 cur_offset;
	unsigned long ptr;

	while (1) {
		ret = btrfs_find_one_extref(fs_root, inum, offset, path, &extref,
					    &offset);
		if (ret < 0)
			break;
		if (ret) {
			ret = found ? 0 : -ENOENT;
			break;
		}
		++found;

		slot = path->slots[0];
		eb = btrfs_clone_extent_buffer(path->nodes[0]);
		if (!eb) {
			ret = -ENOMEM;
			break;
		}
		btrfs_release_path(path);

		item_size = btrfs_item_size_nr(eb, slot);
		ptr = btrfs_item_ptr_offset(eb, slot);
		cur_offset = 0;

		while (cur_offset < item_size) {
			u32 name_len;

			extref = (struct btrfs_inode_extref *)(ptr + cur_offset);
			parent = btrfs_inode_extref_parent(eb, extref);
			name_len = btrfs_inode_extref_name_len(eb, extref);
			ret = iterate(parent, name_len,
				      (unsigned long)&extref->name, eb, ctx);
			if (ret)
				break;

			cur_offset += btrfs_inode_extref_name_len(eb, extref);
			cur_offset += sizeof(*extref);
		}
		free_extent_buffer(eb);

		offset++;
	}

	btrfs_release_path(path);

	return ret;
}

static int iterate_irefs(u64 inum, struct btrfs_root *fs_root,
			 struct btrfs_path *path, iterate_irefs_t *iterate,
			 void *ctx)
{
	int ret;
	int found_refs = 0;

	ret = iterate_inode_refs(inum, fs_root, path, iterate, ctx);
	if (!ret)
		++found_refs;
	else if (ret != -ENOENT)
		return ret;

	ret = iterate_inode_extrefs(inum, fs_root, path, iterate, ctx);
	if (ret == -ENOENT && found_refs)
		return 0;

	return ret;
}

/*
 * returns 0 if the path could be dumped (probably truncated)
 * returns <0 in case of an error
 */
static int inode_to_path(u64 inum, u32 name_len, unsigned long name_off,
			 struct extent_buffer *eb, void *ctx)
{
	struct inode_fs_paths *ipath = ctx;
	char *fspath;
	char *fspath_min;
	int i = ipath->fspath->elem_cnt;
	const int s_ptr = sizeof(char *);
	u32 bytes_left;

	bytes_left = ipath->fspath->bytes_left > s_ptr ?
					ipath->fspath->bytes_left - s_ptr : 0;

	fspath_min = (char *)ipath->fspath->val + (i + 1) * s_ptr;
	fspath = btrfs_ref_to_path(ipath->fs_root, ipath->btrfs_path, name_len,
				   name_off, eb, inum, fspath_min, bytes_left);
	if (IS_ERR(fspath))
		return PTR_ERR(fspath);

	if (fspath > fspath_min) {
		ipath->fspath->val[i] = (u64)(unsigned long)fspath;
		++ipath->fspath->elem_cnt;
		ipath->fspath->bytes_left = fspath - fspath_min;
	} else {
		++ipath->fspath->elem_missed;
		ipath->fspath->bytes_missing += fspath_min - fspath;
		ipath->fspath->bytes_left = 0;
	}

	return 0;
}

/*
 * this dumps all file system paths to the inode into the ipath struct, provided
 * is has been created large enough. each path is zero-terminated and accessed
 * from ipath->fspath->val[i].
 * when it returns, there are ipath->fspath->elem_cnt number of paths available
 * in ipath->fspath->val[]. when the allocated space wasn't sufficient, the
 * number of missed paths is recorded in ipath->fspath->elem_missed, otherwise,
 * it's zero. ipath->fspath->bytes_missing holds the number of bytes that would
 * have been needed to return all paths.
 */
int paths_from_inode(u64 inum, struct inode_fs_paths *ipath)
{
	return iterate_irefs(inum, ipath->fs_root, ipath->btrfs_path,
			     inode_to_path, ipath);
}

struct btrfs_data_container *init_data_container(u32 total_bytes)
{
	struct btrfs_data_container *data;
	size_t alloc_bytes;

	alloc_bytes = max_t(size_t, total_bytes, sizeof(*data));
	data = kvmalloc(alloc_bytes, GFP_KERNEL);
	if (!data)
		return ERR_PTR(-ENOMEM);

	if (total_bytes >= sizeof(*data)) {
		data->bytes_left = total_bytes - sizeof(*data);
		data->bytes_missing = 0;
	} else {
		data->bytes_missing = sizeof(*data) - total_bytes;
		data->bytes_left = 0;
	}

	data->elem_cnt = 0;
	data->elem_missed = 0;

	return data;
}

/*
 * allocates space to return multiple file system paths for an inode.
 * total_bytes to allocate are passed, note that space usable for actual path
 * information will be total_bytes - sizeof(struct inode_fs_paths).
 * the returned pointer must be freed with free_ipath() in the end.
 */
struct inode_fs_paths *init_ipath(s32 total_bytes, struct btrfs_root *fs_root,
					struct btrfs_path *path)
{
	struct inode_fs_paths *ifp;
	struct btrfs_data_container *fspath;

	fspath = init_data_container(total_bytes);
	if (IS_ERR(fspath))
		return ERR_CAST(fspath);

	ifp = kmalloc(sizeof(*ifp), GFP_KERNEL);
	if (!ifp) {
		kvfree(fspath);
		return ERR_PTR(-ENOMEM);
	}

	ifp->btrfs_path = path;
	ifp->fspath = fspath;
	ifp->fs_root = fs_root;

	return ifp;
}

void free_ipath(struct inode_fs_paths *ipath)
{
	if (!ipath)
		return;
	kvfree(ipath->fspath);
	kfree(ipath);
}
