/*
 * Copyright 2018 Red Hat Inc.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE COPYRIGHT HOLDER(S) OR AUTHOR(S) BE LIABLE FOR ANY CLAIM, DAMAGES OR
 * OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE,
 * ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR
 * OTHER DEALINGS IN THE SOFTWARE.
 */
#include "nouveau_svm.h"
#include "nouveau_drv.h"
#include "nouveau_chan.h"
#include "nouveau_dmem.h"

#include <nvif/notify.h>
#include <nvif/object.h>
#include <nvif/vmm.h>

#include <nvif/class.h>
#include <nvif/clb069.h>
#include <nvif/ifc00d.h>

#include <linux/sched/mm.h>
#include <linux/sort.h>
#include <linux/hmm.h>

struct nouveau_svm {
	struct nouveau_drm *drm;
	struct mutex mutex;
	struct list_head inst;

	struct nouveau_svm_fault_buffer {
		int id;
		struct nvif_object object;
		u32 entries;
		u32 getaddr;
		u32 putaddr;
		u32 get;
		u32 put;
		struct nvif_notify notify;

		struct nouveau_svm_fault {
			u64 inst;
			u64 addr;
			u64 time;
			u32 engine;
			u8  gpc;
			u8  hub;
			u8  access;
			u8  client;
			u8  fault;
			struct nouveau_svmm *svmm;
		} **fault;
		int fault_nr;
	} buffer[1];
};

#define SVM_DBG(s,f,a...) NV_DEBUG((s)->drm, "svm: "f"\n", ##a)
#define SVM_ERR(s,f,a...) NV_WARN((s)->drm, "svm: "f"\n", ##a)

struct nouveau_ivmm {
	struct nouveau_svmm *svmm;
	u64 inst;
	struct list_head head;
};

static struct nouveau_ivmm *
nouveau_ivmm_find(struct nouveau_svm *svm, u64 inst)
{
	struct nouveau_ivmm *ivmm;
	list_for_each_entry(ivmm, &svm->inst, head) {
		if (ivmm->inst == inst)
			return ivmm;
	}
	return NULL;
}

struct nouveau_svmm {
	struct nouveau_vmm *vmm;
	struct {
		unsigned long start;
		unsigned long limit;
	} unmanaged;

	struct mutex mutex;

	struct mm_struct *mm;
	struct hmm_mirror mirror;
};

#define SVMM_DBG(s,f,a...)                                                     \
	NV_DEBUG((s)->vmm->cli->drm, "svm-%p: "f"\n", (s), ##a)
#define SVMM_ERR(s,f,a...)                                                     \
	NV_WARN((s)->vmm->cli->drm, "svm-%p: "f"\n", (s), ##a)

int
nouveau_svmm_bind(struct drm_device *dev, void *data,
		  struct drm_file *file_priv)
{
	struct nouveau_cli *cli = nouveau_cli(file_priv);
	struct drm_nouveau_svm_bind *args = data;
	unsigned target, cmd, priority;
	unsigned long addr, end;
	struct mm_struct *mm;

	args->va_start &= PAGE_MASK;
	args->va_end = ALIGN(args->va_end, PAGE_SIZE);

	/* Sanity check arguments */
	if (args->reserved0 || args->reserved1)
		return -EINVAL;
	if (args->header & (~NOUVEAU_SVM_BIND_VALID_MASK))
		return -EINVAL;
	if (args->va_start >= args->va_end)
		return -EINVAL;

	cmd = args->header >> NOUVEAU_SVM_BIND_COMMAND_SHIFT;
	cmd &= NOUVEAU_SVM_BIND_COMMAND_MASK;
	switch (cmd) {
	case NOUVEAU_SVM_BIND_COMMAND__MIGRATE:
		break;
	default:
		return -EINVAL;
	}

	priority = args->header >> NOUVEAU_SVM_BIND_PRIORITY_SHIFT;
	priority &= NOUVEAU_SVM_BIND_PRIORITY_MASK;

	/* FIXME support CPU target ie all target value < GPU_VRAM */
	target = args->header >> NOUVEAU_SVM_BIND_TARGET_SHIFT;
	target &= NOUVEAU_SVM_BIND_TARGET_MASK;
	switch (target) {
	case NOUVEAU_SVM_BIND_TARGET__GPU_VRAM:
		break;
	default:
		return -EINVAL;
	}

	/*
	 * FIXME: For now refuse non 0 stride, we need to change the migrate
	 * kernel function to handle stride to avoid to create a mess within
	 * each device driver.
	 */
	if (args->stride)
		return -EINVAL;

	/*
	 * Ok we are ask to do something sane, for now we only support migrate
	 * commands but we will add things like memory policy (what to do on
	 * page fault) and maybe some other commands.
	 */

	mm = get_task_mm(current);
	down_read(&mm->mmap_sem);

	if (!cli->svm.svmm) {
		up_read(&mm->mmap_sem);
		return -EINVAL;
	}

	for (addr = args->va_start, end = args->va_end; addr < end;) {
		struct vm_area_struct *vma;
		unsigned long next;

		vma = find_vma_intersection(mm, addr, end);
		if (!vma)
			break;

		addr = max(addr, vma->vm_start);
		next = min(vma->vm_end, end);
		/* This is a best effort so we ignore errors */
		nouveau_dmem_migrate_vma(cli->drm, vma, addr, next);
		addr = next;
	}

	/*
	 * FIXME Return the number of page we have migrated, again we need to
	 * update the migrate API to return that information so that we can
	 * report it to user space.
	 */
	args->result = 0;

	up_read(&mm->mmap_sem);
	mmput(mm);

	return 0;
}

/* Unlink channel instance from SVMM. */
void
nouveau_svmm_part(struct nouveau_svmm *svmm, u64 inst)
{
	struct nouveau_ivmm *ivmm;
	if (svmm) {
		mutex_lock(&svmm->vmm->cli->drm->svm->mutex);
		ivmm = nouveau_ivmm_find(svmm->vmm->cli->drm->svm, inst);
		if (ivmm) {
			list_del(&ivmm->head);
			kfree(ivmm);
		}
		mutex_unlock(&svmm->vmm->cli->drm->svm->mutex);
	}
}

/* Link channel instance to SVMM. */
int
nouveau_svmm_join(struct nouveau_svmm *svmm, u64 inst)
{
	struct nouveau_ivmm *ivmm;
	if (svmm) {
		if (!(ivmm = kmalloc(sizeof(*ivmm), GFP_KERNEL)))
			return -ENOMEM;
		ivmm->svmm = svmm;
		ivmm->inst = inst;

		mutex_lock(&svmm->vmm->cli->drm->svm->mutex);
		list_add(&ivmm->head, &svmm->vmm->cli->drm->svm->inst);
		mutex_unlock(&svmm->vmm->cli->drm->svm->mutex);
	}
	return 0;
}

/* Invalidate SVMM address-range on GPU. */
static void
nouveau_svmm_invalidate(struct nouveau_svmm *svmm, u64 start, u64 limit)
{
	if (limit > start) {
		bool super = svmm->vmm->vmm.object.client->super;
		svmm->vmm->vmm.object.client->super = true;
		nvif_object_mthd(&svmm->vmm->vmm.object, NVIF_VMM_V0_PFNCLR,
				 &(struct nvif_vmm_pfnclr_v0) {
					.addr = start,
					.size = limit - start,
				 }, sizeof(struct nvif_vmm_pfnclr_v0));
		svmm->vmm->vmm.object.client->super = super;
	}
}

static int
nouveau_svmm_sync_cpu_device_pagetables(struct hmm_mirror *mirror,
					const struct mmu_notifier_range *update)
{
	struct nouveau_svmm *svmm = container_of(mirror, typeof(*svmm), mirror);
	unsigned long start = update->start;
	unsigned long limit = update->end;

	if (!mmu_notifier_range_blockable(update))
		return -EAGAIN;

	SVMM_DBG(svmm, "invalidate %016lx-%016lx", start, limit);

	mutex_lock(&svmm->mutex);
	if (limit > svmm->unmanaged.start && start < svmm->unmanaged.limit) {
		if (start < svmm->unmanaged.start) {
			nouveau_svmm_invalidate(svmm, start,
						svmm->unmanaged.limit);
		}
		start = svmm->unmanaged.limit;
	}

	nouveau_svmm_invalidate(svmm, start, limit);
	mutex_unlock(&svmm->mutex);
	return 0;
}

static void
nouveau_svmm_release(struct hmm_mirror *mirror)
{
}

static const struct hmm_mirror_ops
nouveau_svmm = {
	.sync_cpu_device_pagetables = nouveau_svmm_sync_cpu_device_pagetables,
	.release = nouveau_svmm_release,
};

void
nouveau_svmm_fini(struct nouveau_svmm **psvmm)
{
	struct nouveau_svmm *svmm = *psvmm;
	if (svmm) {
		hmm_mirror_unregister(&svmm->mirror);
		kfree(*psvmm);
		*psvmm = NULL;
	}
}

int
nouveau_svmm_init(struct drm_device *dev, void *data,
		  struct drm_file *file_priv)
{
	struct nouveau_cli *cli = nouveau_cli(file_priv);
	struct nouveau_svmm *svmm;
	struct drm_nouveau_svm_init *args = data;
	int ret;

	/* Allocate tracking for SVM-enabled VMM. */
	if (!(svmm = kzalloc(sizeof(*svmm), GFP_KERNEL)))
		return -ENOMEM;
	svmm->vmm = &cli->svm;
	svmm->unmanaged.start = args->unmanaged_addr;
	svmm->unmanaged.limit = args->unmanaged_addr + args->unmanaged_size;
	mutex_init(&svmm->mutex);

	/* Check that SVM isn't already enabled for the client. */
	mutex_lock(&cli->mutex);
	if (cli->svm.cli) {
		ret = -EBUSY;
		goto done;
	}

	/* Allocate a new GPU VMM that can support SVM (managed by the
	 * client, with replayable faults enabled).
	 *
	 * All future channel/memory allocations will make use of this
	 * VMM instead of the standard one.
	 */
	ret = nvif_vmm_init(&cli->mmu, cli->vmm.vmm.object.oclass, true,
			    args->unmanaged_addr, args->unmanaged_size,
			    &(struct gp100_vmm_v0) {
				.fault_replay = true,
			    }, sizeof(struct gp100_vmm_v0), &cli->svm.vmm);
	if (ret)
		goto done;

	/* Enable HMM mirroring of CPU address-space to VMM. */
	svmm->mm = get_task_mm(current);
	down_write(&svmm->mm->mmap_sem);
	svmm->mirror.ops = &nouveau_svmm;
	ret = hmm_mirror_register(&svmm->mirror, svmm->mm);
	if (ret == 0) {
		cli->svm.svmm = svmm;
		cli->svm.cli = cli;
	}
	up_write(&svmm->mm->mmap_sem);
	mmput(svmm->mm);

done:
	if (ret)
		nouveau_svmm_fini(&svmm);
	mutex_unlock(&cli->mutex);
	return ret;
}

static const u64
nouveau_svm_pfn_flags[HMM_PFN_FLAG_MAX] = {
	[HMM_PFN_VALID         ] = NVIF_VMM_PFNMAP_V0_V,
	[HMM_PFN_WRITE         ] = NVIF_VMM_PFNMAP_V0_W,
	[HMM_PFN_DEVICE_PRIVATE] = NVIF_VMM_PFNMAP_V0_VRAM,
};

static const u64
nouveau_svm_pfn_values[HMM_PFN_VALUE_MAX] = {
	[HMM_PFN_ERROR  ] = ~NVIF_VMM_PFNMAP_V0_V,
	[HMM_PFN_NONE   ] =  NVIF_VMM_PFNMAP_V0_NONE,
	[HMM_PFN_SPECIAL] = ~NVIF_VMM_PFNMAP_V0_V,
};

/* Issue fault replay for GPU to retry accesses that faulted previously. */
static void
nouveau_svm_fault_replay(struct nouveau_svm *svm)
{
	SVM_DBG(svm, "replay");
	WARN_ON(nvif_object_mthd(&svm->drm->client.vmm.vmm.object,
				 GP100_VMM_VN_FAULT_REPLAY,
				 &(struct gp100_vmm_fault_replay_vn) {},
				 sizeof(struct gp100_vmm_fault_replay_vn)));
}

/* Cancel a replayable fault that could not be handled.
 *
 * Cancelling the fault will trigger recovery to reset the engine
 * and kill the offending channel (ie. GPU SIGSEGV).
 */
static void
nouveau_svm_fault_cancel(struct nouveau_svm *svm,
			 u64 inst, u8 hub, u8 gpc, u8 client)
{
	SVM_DBG(svm, "cancel %016llx %d %02x %02x", inst, hub, gpc, client);
	WARN_ON(nvif_object_mthd(&svm->drm->client.vmm.vmm.object,
				 GP100_VMM_VN_FAULT_CANCEL,
				 &(struct gp100_vmm_fault_cancel_v0) {
					.hub = hub,
					.gpc = gpc,
					.client = client,
					.inst = inst,
				 }, sizeof(struct gp100_vmm_fault_cancel_v0)));
}

static void
nouveau_svm_fault_cancel_fault(struct nouveau_svm *svm,
			       struct nouveau_svm_fault *fault)
{
	nouveau_svm_fault_cancel(svm, fault->inst,
				      fault->hub,
				      fault->gpc,
				      fault->client);
}

static int
nouveau_svm_fault_cmp(const void *a, const void *b)
{
	const struct nouveau_svm_fault *fa = *(struct nouveau_svm_fault **)a;
	const struct nouveau_svm_fault *fb = *(struct nouveau_svm_fault **)b;
	int ret;
	if ((ret = (s64)fa->inst - fb->inst))
		return ret;
	if ((ret = (s64)fa->addr - fb->addr))
		return ret;
	/*XXX: atomic? */
	return (fa->access == 0 || fa->access == 3) -
	       (fb->access == 0 || fb->access == 3);
}

static void
nouveau_svm_fault_cache(struct nouveau_svm *svm,
			struct nouveau_svm_fault_buffer *buffer, u32 offset)
{
	struct nvif_object *memory = &buffer->object;
	const u32 instlo = nvif_rd32(memory, offset + 0x00);
	const u32 insthi = nvif_rd32(memory, offset + 0x04);
	const u32 addrlo = nvif_rd32(memory, offset + 0x08);
	const u32 addrhi = nvif_rd32(memory, offset + 0x0c);
	const u32 timelo = nvif_rd32(memory, offset + 0x10);
	const u32 timehi = nvif_rd32(memory, offset + 0x14);
	const u32 engine = nvif_rd32(memory, offset + 0x18);
	const u32   info = nvif_rd32(memory, offset + 0x1c);
	const u64   inst = (u64)insthi << 32 | instlo;
	const u8     gpc = (info & 0x1f000000) >> 24;
	const u8     hub = (info & 0x00100000) >> 20;
	const u8  client = (info & 0x00007f00) >> 8;
	struct nouveau_svm_fault *fault;

	//XXX: i think we're supposed to spin waiting */
	if (WARN_ON(!(info & 0x80000000)))
		return;

	nvif_mask(memory, offset + 0x1c, 0x80000000, 0x00000000);

	if (!buffer->fault[buffer->fault_nr]) {
		fault = kmalloc(sizeof(*fault), GFP_KERNEL);
		if (WARN_ON(!fault)) {
			nouveau_svm_fault_cancel(svm, inst, hub, gpc, client);
			return;
		}
		buffer->fault[buffer->fault_nr] = fault;
	}

	fault = buffer->fault[buffer->fault_nr++];
	fault->inst   = inst;
	fault->addr   = (u64)addrhi << 32 | addrlo;
	fault->time   = (u64)timehi << 32 | timelo;
	fault->engine = engine;
	fault->gpc    = gpc;
	fault->hub    = hub;
	fault->access = (info & 0x000f0000) >> 16;
	fault->client = client;
	fault->fault  = (info & 0x0000001f);

	SVM_DBG(svm, "fault %016llx %016llx %02x",
		fault->inst, fault->addr, fault->access);
}

static inline bool
nouveau_range_done(struct hmm_range *range)
{
	bool ret = hmm_range_valid(range);

	hmm_range_unregister(range);
	return ret;
}

static int
nouveau_range_fault(struct nouveau_svmm *svmm, struct hmm_range *range)
{
	long ret;

	range->default_flags = 0;
	range->pfn_flags_mask = -1UL;

	ret = hmm_range_register(range, &svmm->mirror);
	if (ret) {
		up_read(&svmm->mm->mmap_sem);
		return (int)ret;
	}

	if (!hmm_range_wait_until_valid(range, HMM_RANGE_DEFAULT_TIMEOUT)) {
		up_read(&svmm->mm->mmap_sem);
		return -EBUSY;
	}

	ret = hmm_range_fault(range, 0);
	if (ret <= 0) {
		if (ret == 0)
			ret = -EBUSY;
		up_read(&svmm->mm->mmap_sem);
		hmm_range_unregister(range);
		return ret;
	}
	return 0;
}

static int
nouveau_svm_fault(struct nvif_notify *notify)
{
	struct nouveau_svm_fault_buffer *buffer =
		container_of(notify, typeof(*buffer), notify);
	struct nouveau_svm *svm =
		container_of(buffer, typeof(*svm), buffer[buffer->id]);
	struct nvif_object *device = &svm->drm->client.device.object;
	struct nouveau_svmm *svmm;
	struct {
		struct {
			struct nvif_ioctl_v0 i;
			struct nvif_ioctl_mthd_v0 m;
			struct nvif_vmm_pfnmap_v0 p;
		} i;
		u64 phys[16];
	} args;
	struct hmm_range range;
	struct vm_area_struct *vma;
	u64 inst, start, limit;
	int fi, fn, pi, fill;
	int replay = 0, ret;

	/* Parse available fault buffer entries into a cache, and update
	 * the GET pointer so HW can reuse the entries.
	 */
	SVM_DBG(svm, "fault handler");
	if (buffer->get == buffer->put) {
		buffer->put = nvif_rd32(device, buffer->putaddr);
		buffer->get = nvif_rd32(device, buffer->getaddr);
		if (buffer->get == buffer->put)
			return NVIF_NOTIFY_KEEP;
	}
	buffer->fault_nr = 0;

	SVM_DBG(svm, "get %08x put %08x", buffer->get, buffer->put);
	while (buffer->get != buffer->put) {
		nouveau_svm_fault_cache(svm, buffer, buffer->get * 0x20);
		if (++buffer->get == buffer->entries)
			buffer->get = 0;
	}
	nvif_wr32(device, buffer->getaddr, buffer->get);
	SVM_DBG(svm, "%d fault(s) pending", buffer->fault_nr);

	/* Sort parsed faults by instance pointer to prevent unnecessary
	 * instance to SVMM translations, followed by address and access
	 * type to reduce the amount of work when handling the faults.
	 */
	sort(buffer->fault, buffer->fault_nr, sizeof(*buffer->fault),
	     nouveau_svm_fault_cmp, NULL);

	/* Lookup SVMM structure for each unique instance pointer. */
	mutex_lock(&svm->mutex);
	for (fi = 0, svmm = NULL; fi < buffer->fault_nr; fi++) {
		if (!svmm || buffer->fault[fi]->inst != inst) {
			struct nouveau_ivmm *ivmm =
				nouveau_ivmm_find(svm, buffer->fault[fi]->inst);
			svmm = ivmm ? ivmm->svmm : NULL;
			inst = buffer->fault[fi]->inst;
			SVM_DBG(svm, "inst %016llx -> svm-%p", inst, svmm);
		}
		buffer->fault[fi]->svmm = svmm;
	}
	mutex_unlock(&svm->mutex);

	/* Process list of faults. */
	args.i.i.version = 0;
	args.i.i.type = NVIF_IOCTL_V0_MTHD;
	args.i.m.version = 0;
	args.i.m.method = NVIF_VMM_V0_PFNMAP;
	args.i.p.version = 0;

	for (fi = 0; fn = fi + 1, fi < buffer->fault_nr; fi = fn) {
		/* Cancel any faults from non-SVM channels. */
		if (!(svmm = buffer->fault[fi]->svmm)) {
			nouveau_svm_fault_cancel_fault(svm, buffer->fault[fi]);
			continue;
		}
		SVMM_DBG(svmm, "addr %016llx", buffer->fault[fi]->addr);

		/* We try and group handling of faults within a small
		 * window into a single update.
		 */
		start = buffer->fault[fi]->addr;
		limit = start + (ARRAY_SIZE(args.phys) << PAGE_SHIFT);
		if (start < svmm->unmanaged.limit)
			limit = min_t(u64, limit, svmm->unmanaged.start);
		else
		if (limit > svmm->unmanaged.start)
			start = max_t(u64, start, svmm->unmanaged.limit);
		SVMM_DBG(svmm, "wndw %016llx-%016llx", start, limit);

		/* Intersect fault window with the CPU VMA, cancelling
		 * the fault if the address is invalid.
		 */
		down_read(&svmm->mm->mmap_sem);
		vma = find_vma_intersection(svmm->mm, start, limit);
		if (!vma) {
			SVMM_ERR(svmm, "wndw %016llx-%016llx", start, limit);
			up_read(&svmm->mm->mmap_sem);
			nouveau_svm_fault_cancel_fault(svm, buffer->fault[fi]);
			continue;
		}
		start = max_t(u64, start, vma->vm_start);
		limit = min_t(u64, limit, vma->vm_end);
		SVMM_DBG(svmm, "wndw %016llx-%016llx", start, limit);

		if (buffer->fault[fi]->addr != start) {
			SVMM_ERR(svmm, "addr %016llx", buffer->fault[fi]->addr);
			up_read(&svmm->mm->mmap_sem);
			nouveau_svm_fault_cancel_fault(svm, buffer->fault[fi]);
			continue;
		}

		/* Prepare the GPU-side update of all pages within the
		 * fault window, determining required pages and access
		 * permissions based on pending faults.
		 */
		args.i.p.page = PAGE_SHIFT;
		args.i.p.addr = start;
		for (fn = fi, pi = 0;;) {
			/* Determine required permissions based on GPU fault
			 * access flags.
			 *XXX: atomic?
			 */
			if (buffer->fault[fn]->access != 0 /* READ. */ &&
			    buffer->fault[fn]->access != 3 /* PREFETCH. */) {
				args.phys[pi++] = NVIF_VMM_PFNMAP_V0_V |
						  NVIF_VMM_PFNMAP_V0_W;
			} else {
				args.phys[pi++] = NVIF_VMM_PFNMAP_V0_V;
			}
			args.i.p.size = pi << PAGE_SHIFT;

			/* It's okay to skip over duplicate addresses from the
			 * same SVMM as faults are ordered by access type such
			 * that only the first one needs to be handled.
			 *
			 * ie. WRITE faults appear first, thus any handling of
			 * pending READ faults will already be satisfied.
			 */
			while (++fn < buffer->fault_nr &&
			       buffer->fault[fn]->svmm == svmm &&
			       buffer->fault[fn    ]->addr ==
			       buffer->fault[fn - 1]->addr);

			/* If the next fault is outside the window, or all GPU
			 * faults have been dealt with, we're done here.
			 */
			if (fn >= buffer->fault_nr ||
			    buffer->fault[fn]->svmm != svmm ||
			    buffer->fault[fn]->addr >= limit)
				break;

			/* Fill in the gap between this fault and the next. */
			fill = (buffer->fault[fn    ]->addr -
				buffer->fault[fn - 1]->addr) >> PAGE_SHIFT;
			while (--fill)
				args.phys[pi++] = NVIF_VMM_PFNMAP_V0_NONE;
		}

		SVMM_DBG(svmm, "wndw %016llx-%016llx covering %d fault(s)",
			 args.i.p.addr,
			 args.i.p.addr + args.i.p.size, fn - fi);

		/* Have HMM fault pages within the fault window to the GPU. */
		range.start = args.i.p.addr;
		range.end = args.i.p.addr + args.i.p.size;
		range.pfns = args.phys;
		range.flags = nouveau_svm_pfn_flags;
		range.values = nouveau_svm_pfn_values;
		range.pfn_shift = NVIF_VMM_PFNMAP_V0_ADDR_SHIFT;
again:
		ret = nouveau_range_fault(svmm, &range);
		if (ret == 0) {
			mutex_lock(&svmm->mutex);
			if (!nouveau_range_done(&range)) {
				mutex_unlock(&svmm->mutex);
				goto again;
			}

			nouveau_dmem_convert_pfn(svm->drm, &range);

			svmm->vmm->vmm.object.client->super = true;
			ret = nvif_object_ioctl(&svmm->vmm->vmm.object,
						&args, sizeof(args.i) +
						pi * sizeof(args.phys[0]),
						NULL);
			svmm->vmm->vmm.object.client->super = false;
			mutex_unlock(&svmm->mutex);
			up_read(&svmm->mm->mmap_sem);
		}

		/* Cancel any faults in the window whose pages didn't manage
		 * to keep their valid bit, or stay writeable when required.
		 *
		 * If handling failed completely, cancel all faults.
		 */
		while (fi < fn) {
			struct nouveau_svm_fault *fault = buffer->fault[fi++];
			pi = (fault->addr - range.start) >> PAGE_SHIFT;
			if (ret ||
			     !(range.pfns[pi] & NVIF_VMM_PFNMAP_V0_V) ||
			    (!(range.pfns[pi] & NVIF_VMM_PFNMAP_V0_W) &&
			     fault->access != 0 && fault->access != 3)) {
				nouveau_svm_fault_cancel_fault(svm, fault);
				continue;
			}
			replay++;
		}
	}

	/* Issue fault replay to the GPU. */
	if (replay)
		nouveau_svm_fault_replay(svm);
	return NVIF_NOTIFY_KEEP;
}

static void
nouveau_svm_fault_buffer_fini(struct nouveau_svm *svm, int id)
{
	struct nouveau_svm_fault_buffer *buffer = &svm->buffer[id];
	nvif_notify_put(&buffer->notify);
}

static int
nouveau_svm_fault_buffer_init(struct nouveau_svm *svm, int id)
{
	struct nouveau_svm_fault_buffer *buffer = &svm->buffer[id];
	struct nvif_object *device = &svm->drm->client.device.object;
	buffer->get = nvif_rd32(device, buffer->getaddr);
	buffer->put = nvif_rd32(device, buffer->putaddr);
	SVM_DBG(svm, "get %08x put %08x (init)", buffer->get, buffer->put);
	return nvif_notify_get(&buffer->notify);
}

static void
nouveau_svm_fault_buffer_dtor(struct nouveau_svm *svm, int id)
{
	struct nouveau_svm_fault_buffer *buffer = &svm->buffer[id];
	int i;

	if (buffer->fault) {
		for (i = 0; buffer->fault[i] && i < buffer->entries; i++)
			kfree(buffer->fault[i]);
		kvfree(buffer->fault);
	}

	nouveau_svm_fault_buffer_fini(svm, id);

	nvif_notify_fini(&buffer->notify);
	nvif_object_fini(&buffer->object);
}

static int
nouveau_svm_fault_buffer_ctor(struct nouveau_svm *svm, s32 oclass, int id)
{
	struct nouveau_svm_fault_buffer *buffer = &svm->buffer[id];
	struct nouveau_drm *drm = svm->drm;
	struct nvif_object *device = &drm->client.device.object;
	struct nvif_clb069_v0 args = {};
	int ret;

	buffer->id = id;

	ret = nvif_object_init(device, 0, oclass, &args, sizeof(args),
			       &buffer->object);
	if (ret < 0) {
		SVM_ERR(svm, "Fault buffer allocation failed: %d", ret);
		return ret;
	}

	nvif_object_map(&buffer->object, NULL, 0);
	buffer->entries = args.entries;
	buffer->getaddr = args.get;
	buffer->putaddr = args.put;

	ret = nvif_notify_init(&buffer->object, nouveau_svm_fault, true,
			       NVB069_V0_NTFY_FAULT, NULL, 0, 0,
			       &buffer->notify);
	if (ret)
		return ret;

	buffer->fault = kvzalloc(sizeof(*buffer->fault) * buffer->entries, GFP_KERNEL);
	if (!buffer->fault)
		return -ENOMEM;

	return nouveau_svm_fault_buffer_init(svm, id);
}

void
nouveau_svm_resume(struct nouveau_drm *drm)
{
	struct nouveau_svm *svm = drm->svm;
	if (svm)
		nouveau_svm_fault_buffer_init(svm, 0);
}

void
nouveau_svm_suspend(struct nouveau_drm *drm)
{
	struct nouveau_svm *svm = drm->svm;
	if (svm)
		nouveau_svm_fault_buffer_fini(svm, 0);
}

void
nouveau_svm_fini(struct nouveau_drm *drm)
{
	struct nouveau_svm *svm = drm->svm;
	if (svm) {
		nouveau_svm_fault_buffer_dtor(svm, 0);
		kfree(drm->svm);
		drm->svm = NULL;
	}
}

void
nouveau_svm_init(struct nouveau_drm *drm)
{
	static const struct nvif_mclass buffers[] = {
		{   VOLTA_FAULT_BUFFER_A, 0 },
		{ MAXWELL_FAULT_BUFFER_A, 0 },
		{}
	};
	struct nouveau_svm *svm;
	int ret;

	/* Disable on Volta and newer until channel recovery is fixed,
	 * otherwise clients will have a trivial way to trash the GPU
	 * for everyone.
	 */
	if (drm->client.device.info.family > NV_DEVICE_INFO_V0_PASCAL)
		return;

	if (!(drm->svm = svm = kzalloc(sizeof(*drm->svm), GFP_KERNEL)))
		return;

	drm->svm->drm = drm;
	mutex_init(&drm->svm->mutex);
	INIT_LIST_HEAD(&drm->svm->inst);

	ret = nvif_mclass(&drm->client.device.object, buffers);
	if (ret < 0) {
		SVM_DBG(svm, "No supported fault buffer class");
		nouveau_svm_fini(drm);
		return;
	}

	ret = nouveau_svm_fault_buffer_ctor(svm, buffers[ret].oclass, 0);
	if (ret) {
		nouveau_svm_fini(drm);
		return;
	}

	SVM_DBG(svm, "Initialised");
}
