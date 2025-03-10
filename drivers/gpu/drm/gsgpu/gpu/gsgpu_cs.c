#include <linux/pagemap.h>
#include <linux/sync_file.h>
#include <drm/gsgpu_drm.h>
#include <drm/drm_syncobj.h>
#include "gsgpu.h"
#include "gsgpu_vm_it.h"
#include "gsgpu_trace.h"
#include "gsgpu_gmc.h"

static int gsgpu_cs_user_fence_chunk(struct gsgpu_cs_parser *p,
				      struct drm_gsgpu_cs_chunk_fence *data,
				      uint32_t *offset)
{
	struct drm_gem_object *gobj;
	unsigned long size;
	int r;

	gobj = drm_gem_object_lookup(p->filp, data->handle);
	if (gobj == NULL)
		return -EINVAL;

	p->uf_entry.robj = gsgpu_bo_ref(gem_to_gsgpu_bo(gobj));
	p->uf_entry.priority = 0;
	p->uf_entry.tv.bo = &p->uf_entry.robj->tbo;
	p->uf_entry.tv.num_shared = 1;

	drm_gem_object_put(gobj);

	size = gsgpu_bo_size(p->uf_entry.robj);
	if (size != PAGE_SIZE || (data->offset + 8) > size) {
		r = -EINVAL;
		goto error_unref;
	}

	if (gsgpu_ttm_tt_get_usermm(p->uf_entry.robj->tbo.ttm)) {
		r = -EINVAL;
		goto error_unref;
	}

	*offset = data->offset;

	return 0;

error_unref:
	gsgpu_bo_unref(&p->uf_entry.robj);
	return r;
}

static int gsgpu_cs_bo_handles_chunk(struct gsgpu_cs_parser *p,
				      struct drm_gsgpu_bo_list_in *data)
{
	int r;
	struct drm_gsgpu_bo_list_entry *info = NULL;

	r = gsgpu_bo_create_list_entry_array(data, &info);
	if (r)
		return r;

	r = gsgpu_bo_list_create(p->adev, p->filp, info, data->bo_number,
				  &p->bo_list);
	if (r)
		goto error_free;

	kvfree(info);
	return 0;

error_free:
	if (info)
		kvfree(info);

	return r;
}

static int gsgpu_cs_parser_init(struct gsgpu_cs_parser *p, union drm_gsgpu_cs *cs)
{
	struct gsgpu_fpriv *fpriv = p->filp->driver_priv;
	struct gsgpu_vm *vm = &fpriv->vm;
	uint64_t *chunk_array_user;
	uint64_t *chunk_array;
	unsigned size, num_ibs = 0;
	uint32_t uf_offset = 0;
	int i;
	int ret;

	if (cs->in.num_chunks == 0)
		return 0;

	chunk_array = kmalloc_array(cs->in.num_chunks, sizeof(uint64_t), GFP_KERNEL);
	if (!chunk_array)
		return -ENOMEM;

	p->ctx = gsgpu_ctx_get(fpriv, cs->in.ctx_id);
	if (!p->ctx) {
		ret = -EINVAL;
		goto free_chunk;
	}

	mutex_lock(&p->ctx->lock);

	/* skip guilty context job */
	if (atomic_read(&p->ctx->guilty) == 1) {
		ret = -ECANCELED;
		goto free_chunk;
	}

	/* get chunks */
	chunk_array_user = u64_to_user_ptr(cs->in.chunks);
	if (copy_from_user(chunk_array, chunk_array_user,
			   sizeof(uint64_t)*cs->in.num_chunks)) {
		ret = -EFAULT;
		goto free_chunk;
	}

	p->nchunks = cs->in.num_chunks;
	p->chunks = kmalloc_array(p->nchunks, sizeof(struct gsgpu_cs_chunk),
			    GFP_KERNEL);
	if (!p->chunks) {
		ret = -ENOMEM;
		goto free_chunk;
	}

	for (i = 0; i < p->nchunks; i++) {
		struct drm_gsgpu_cs_chunk __user **chunk_ptr = NULL;
		struct drm_gsgpu_cs_chunk user_chunk;
		uint32_t __user *cdata;

		chunk_ptr = u64_to_user_ptr(chunk_array[i]);
		if (copy_from_user(&user_chunk, chunk_ptr,
				       sizeof(struct drm_gsgpu_cs_chunk))) {
			ret = -EFAULT;
			i--;
			goto free_partial_kdata;
		}
		p->chunks[i].chunk_id = user_chunk.chunk_id;
		p->chunks[i].length_dw = user_chunk.length_dw;

		size = p->chunks[i].length_dw;
		cdata = u64_to_user_ptr(user_chunk.chunk_data);

		p->chunks[i].kdata = kvmalloc_array(size, sizeof(uint32_t), GFP_KERNEL);
		if (p->chunks[i].kdata == NULL) {
			ret = -ENOMEM;
			i--;
			goto free_partial_kdata;
		}
		size *= sizeof(uint32_t);
		if (copy_from_user(p->chunks[i].kdata, cdata, size)) {
			ret = -EFAULT;
			goto free_partial_kdata;
		}

		switch (p->chunks[i].chunk_id) {
		case GSGPU_CHUNK_ID_IB:
			++num_ibs;
			break;

		case GSGPU_CHUNK_ID_FENCE:
			size = sizeof(struct drm_gsgpu_cs_chunk_fence);
			if (p->chunks[i].length_dw * sizeof(uint32_t) < size) {
				ret = -EINVAL;
				goto free_partial_kdata;
			}

			ret = gsgpu_cs_user_fence_chunk(p, p->chunks[i].kdata,
							 &uf_offset);
			if (ret)
				goto free_partial_kdata;

			break;

		case GSGPU_CHUNK_ID_BO_HANDLES:
			size = sizeof(struct drm_gsgpu_bo_list_in);
			if (p->chunks[i].length_dw * sizeof(uint32_t) < size) {
				ret = -EINVAL;
				goto free_partial_kdata;
			}

			ret = gsgpu_cs_bo_handles_chunk(p, p->chunks[i].kdata);
			if (ret)
				goto free_partial_kdata;

			break;

		case GSGPU_CHUNK_ID_DEPENDENCIES:
		case GSGPU_CHUNK_ID_SYNCOBJ_IN:
		case GSGPU_CHUNK_ID_SYNCOBJ_OUT:
			break;

		default:
			ret = -EINVAL;
			goto free_partial_kdata;
		}
	}

	ret = gsgpu_job_alloc(p->adev, num_ibs, &p->job, vm);
	if (ret)
		goto free_all_kdata;

	if (p->ctx->vram_lost_counter != p->job->vram_lost_counter) {
		ret = -ECANCELED;
		goto free_all_kdata;
	}

	if (p->uf_entry.robj)
		p->job->uf_addr = uf_offset;
	kfree(chunk_array);

	/* Use this opportunity to fill in task info for the vm */
	gsgpu_vm_set_task_info(vm);

	return 0;

free_all_kdata:
	i = p->nchunks - 1;
free_partial_kdata:
	for (; i >= 0; i--)
		kvfree(p->chunks[i].kdata);
	kfree(p->chunks);
	p->chunks = NULL;
	p->nchunks = 0;
free_chunk:
	kfree(chunk_array);

	return ret;
}

/* Convert microseconds to bytes. */
static u64 us_to_bytes(struct gsgpu_device *adev, s64 us)
{
	if (us <= 0 || !adev->mm_stats.log2_max_MBps)
		return 0;

	/* Since accum_us is incremented by a million per second, just
	 * multiply it by the number of MB/s to get the number of bytes.
	 */
	return us << adev->mm_stats.log2_max_MBps;
}

static s64 bytes_to_us(struct gsgpu_device *adev, u64 bytes)
{
	if (!adev->mm_stats.log2_max_MBps)
		return 0;

	return bytes >> adev->mm_stats.log2_max_MBps;
}

/* Returns how many bytes TTM can move right now. If no bytes can be moved,
 * it returns 0. If it returns non-zero, it's OK to move at least one buffer,
 * which means it can go over the threshold once. If that happens, the driver
 * will be in debt and no other buffer migrations can be done until that debt
 * is repaid.
 *
 * This approach allows moving a buffer of any size (it's important to allow
 * that).
 *
 * The currency is simply time in microseconds and it increases as the clock
 * ticks. The accumulated microseconds (us) are converted to bytes and
 * returned.
 */
static void gsgpu_cs_get_threshold_for_moves(struct gsgpu_device *adev,
					      u64 *max_bytes,
					      u64 *max_vis_bytes)
{
	s64 time_us, increment_us;
	u64 free_vram, total_vram, used_vram;

	/* Allow a maximum of 200 accumulated ms. This is basically per-IB
	 * throttling.
	 *
	 * It means that in order to get full max MBps, at least 5 IBs per
	 * second must be submitted and not more than 200ms apart from each
	 * other.
	 */
	const s64 us_upper_bound = 200000;

	if (!adev->mm_stats.log2_max_MBps) {
		*max_bytes = 0;
		*max_vis_bytes = 0;
		return;
	}

	total_vram = adev->gmc.real_vram_size - atomic64_read(&adev->vram_pin_size);
	used_vram = gsgpu_vram_mgr_usage(&adev->mman.vram_mgr);
	free_vram = used_vram >= total_vram ? 0 : total_vram - used_vram;

	spin_lock(&adev->mm_stats.lock);

	/* Increase the amount of accumulated us. */
	time_us = ktime_to_us(ktime_get());
	increment_us = time_us - adev->mm_stats.last_update_us;
	adev->mm_stats.last_update_us = time_us;
	adev->mm_stats.accum_us = min(adev->mm_stats.accum_us + increment_us,
						us_upper_bound);

	/* This prevents the short period of low performance when the VRAM
	 * usage is low and the driver is in debt or doesn't have enough
	 * accumulated us to fill VRAM quickly.
	 *
	 * The situation can occur in these cases:
	 * - a lot of VRAM is freed by userspace
	 * - the presence of a big buffer causes a lot of evictions
	 *   (solution: split buffers into smaller ones)
	 *
	 * If 128 MB or 1/8th of VRAM is free, start filling it now by setting
	 * accum_us to a positive number.
	 */
	if (free_vram >= 128 * 1024 * 1024 || free_vram >= total_vram / 8) {
		s64 min_us;

		/* Be more aggresive on dGPUs. Try to fill a portion of free
		 * VRAM now.
		 */
		if (!(adev->flags & GSGPU_IS_APU))
			min_us = bytes_to_us(adev, free_vram / 4);
		else
			min_us = 0; /* Reset accum_us on APUs. */

		adev->mm_stats.accum_us = max(min_us, adev->mm_stats.accum_us);
	}

	/* This is set to 0 if the driver is in debt to disallow (optional)
	 * buffer moves.
	 */
	*max_bytes = us_to_bytes(adev, adev->mm_stats.accum_us);

	/* Do the same for visible VRAM if half of it is free */
	if (!gsgpu_gmc_vram_full_visible(&adev->gmc)) {
		u64 total_vis_vram = adev->gmc.visible_vram_size;
		u64 used_vis_vram = gsgpu_vram_mgr_vis_usage(&adev->mman.vram_mgr);

		if (used_vis_vram < total_vis_vram) {
			u64 free_vis_vram = total_vis_vram - used_vis_vram;
			adev->mm_stats.accum_us_vis = min(adev->mm_stats.accum_us_vis +
							  increment_us, us_upper_bound);

			if (free_vis_vram >= total_vis_vram / 2)
				adev->mm_stats.accum_us_vis =
					max(bytes_to_us(adev, free_vis_vram / 2),
					    adev->mm_stats.accum_us_vis);
		}

		*max_vis_bytes = us_to_bytes(adev, adev->mm_stats.accum_us_vis);
	} else {
		*max_vis_bytes = 0;
	}

	spin_unlock(&adev->mm_stats.lock);
}

/* Report how many bytes have really been moved for the last command
 * submission. This can result in a debt that can stop buffer migrations
 * temporarily.
 */
void gsgpu_cs_report_moved_bytes(struct gsgpu_device *adev, u64 num_bytes,
				  u64 num_vis_bytes)
{
	spin_lock(&adev->mm_stats.lock);
	adev->mm_stats.accum_us -= bytes_to_us(adev, num_bytes);
	adev->mm_stats.accum_us_vis -= bytes_to_us(adev, num_vis_bytes);
	spin_unlock(&adev->mm_stats.lock);
}

static int gsgpu_cs_bo_validate(struct gsgpu_cs_parser *p,
				 struct gsgpu_bo *bo)
{
	struct gsgpu_device *adev = gsgpu_ttm_adev(bo->tbo.bdev);
	struct ttm_operation_ctx ctx = {
		.interruptible = true,
		.no_wait_gpu = false,
		.resv = bo->tbo.base.resv,
	};
	uint32_t domain;
	int r;

	if (bo->tbo.pin_count)
		return 0;

	/* Don't move this buffer if we have depleted our allowance
	 * to move it. Don't move anything if the threshold is zero.
	 */
	if (p->bytes_moved < p->bytes_moved_threshold) {
		if (!gsgpu_gmc_vram_full_visible(&adev->gmc) &&
		    (bo->flags & GSGPU_GEM_CREATE_CPU_ACCESS_REQUIRED)) {
			/* And don't move a CPU_ACCESS_REQUIRED BO to limited
			 * visible VRAM if we've depleted our allowance to do
			 * that.
			 */
			if (p->bytes_moved_vis < p->bytes_moved_vis_threshold)
				domain = bo->preferred_domains;
			else
				domain = bo->allowed_domains;
		} else {
			domain = bo->preferred_domains;
		}
	} else {
		domain = bo->allowed_domains;
	}

retry:
	gsgpu_bo_placement_from_domain(bo, domain);
	r = ttm_bo_validate(&bo->tbo, &bo->placement, &ctx);

	p->bytes_moved += ctx.bytes_moved;
	if (!gsgpu_gmc_vram_full_visible(&adev->gmc) &&
	    gsgpu_bo_in_cpu_visible_vram(bo))
		p->bytes_moved_vis += ctx.bytes_moved;

	if (unlikely(r == -ENOMEM) && domain != bo->allowed_domains) {
		domain = bo->allowed_domains;
		goto retry;
	}

	return r;
}

/* Last resort, try to evict something from the current working set */
static bool gsgpu_cs_try_evict(struct gsgpu_cs_parser *p,
				struct gsgpu_bo *validated)
{
	uint32_t domain = validated->allowed_domains;
	struct ttm_operation_ctx ctx = { true, false };
	int r;

	if (!p->evictable)
		return false;

	for (; &p->evictable->tv.head != &p->validated;
	     p->evictable = list_prev_entry(p->evictable, tv.head)) {

		struct gsgpu_bo_list_entry *candidate = p->evictable;
		struct gsgpu_bo *bo = candidate->robj;
		struct gsgpu_device *adev = gsgpu_ttm_adev(bo->tbo.bdev);
		bool update_bytes_moved_vis;
		uint32_t other;

		/* If we reached our current BO we can forget it */
		if (candidate->robj == validated)
			break;

		/* We can't move pinned BOs here */
		if (bo->tbo.pin_count)
			continue;

		other = gsgpu_mem_type_to_domain(bo->tbo.resource->mem_type);

		/* Check if this BO is in one of the domains we need space for */
		if (!(other & domain))
			continue;

		/* Check if we can move this BO somewhere else */
		other = bo->allowed_domains & ~domain;
		if (!other)
			continue;

		/* Good we can try to move this BO somewhere else */
		update_bytes_moved_vis =
				!gsgpu_gmc_vram_full_visible(&adev->gmc) &&
				gsgpu_bo_in_cpu_visible_vram(bo);
		gsgpu_bo_placement_from_domain(bo, other);
		r = ttm_bo_validate(&bo->tbo, &bo->placement, &ctx);
		p->bytes_moved += ctx.bytes_moved;
		if (update_bytes_moved_vis)
			p->bytes_moved_vis += ctx.bytes_moved;

		if (unlikely(r))
			break;

		p->evictable = list_prev_entry(p->evictable, tv.head);
		list_move(&candidate->tv.head, &p->validated);

		return true;
	}

	return false;
}

static int gsgpu_cs_validate(void *param, struct gsgpu_bo *bo)
{
	struct gsgpu_cs_parser *p = param;
	int r;

	do {
		r = gsgpu_cs_bo_validate(p, bo);
	} while (r == -ENOMEM && gsgpu_cs_try_evict(p, bo));
	if (r)
		return r;

	if (bo->shadow)
		r = gsgpu_cs_bo_validate(p, bo->shadow);

	return r;
}

static int gsgpu_cs_list_validate(struct gsgpu_cs_parser *p,
			    struct list_head *validated)
{
	struct ttm_operation_ctx ctx = { true, false };
	struct gsgpu_bo_list_entry *lobj;
	int r;

	list_for_each_entry(lobj, validated, tv.head) {
		struct gsgpu_bo *bo = lobj->robj;
		bool binding_userptr = false;
		struct mm_struct *usermm;

		usermm = gsgpu_ttm_tt_get_usermm(bo->tbo.ttm);
		if (usermm && usermm != current->mm)
			return -EPERM;

		/* Check if we have user pages and nobody bound the BO already */
		if (gsgpu_ttm_tt_is_userptr(bo->tbo.ttm) &&
		    lobj->user_invalidated && lobj->user_pages) {
			gsgpu_bo_placement_from_domain(bo, GSGPU_GEM_DOMAIN_CPU);
			r = ttm_bo_validate(&bo->tbo, &bo->placement, &ctx);
			if (r)
				return r;
			gsgpu_ttm_tt_set_user_pages(bo->tbo.ttm, lobj->user_pages);
			binding_userptr = true;
		}

		if (p->evictable == lobj)
			p->evictable = NULL;

		r = gsgpu_cs_validate(p, bo);
		if (r)
			return r;

		if (binding_userptr) {
			kvfree(lobj->user_pages);
			lobj->user_pages = NULL;
		}
	}
	return 0;
}

static int gsgpu_cs_parser_bos(struct gsgpu_cs_parser *p,
			       union drm_gsgpu_cs *cs)
{
	struct gsgpu_fpriv *fpriv = p->filp->driver_priv;
	struct gsgpu_vm *vm = &fpriv->vm;
	struct gsgpu_bo_list_entry *e;
	struct list_head duplicates;
	int r;

	INIT_LIST_HEAD(&p->validated);

	/* p->bo_list could already be assigned if GSGPU_CHUNK_ID_BO_HANDLES is present */
	if (cs->in.bo_list_handle) {
		if (p->bo_list)
			return -EINVAL;

		r = gsgpu_bo_list_get(fpriv, cs->in.bo_list_handle,
				       &p->bo_list);
		if (r)
			return r;
	} else if (!p->bo_list) {
		/* Create a empty bo_list when no handle is provided */
		r = gsgpu_bo_list_create(p->adev, p->filp, NULL, 0,
					  &p->bo_list);
		if (r)
			return r;
	}

	gsgpu_bo_list_get_list(p->bo_list, &p->validated);

	INIT_LIST_HEAD(&duplicates);
	gsgpu_vm_get_pd_bo(&fpriv->vm, &p->validated, &p->vm_pd);

	if (p->uf_entry.robj && !p->uf_entry.robj->parent)
		list_add(&p->uf_entry.tv.head, &p->validated);

	/* Get userptr backing pages. If pages are updated after registered
	 * in gsgpu_gem_userptr_ioctl(), gsgpu_cs_list_validate() will do
	 * gsgpu_ttm_backend_bind() to flush and invalidate new pages
	 */
	gsgpu_bo_list_for_each_userptr_entry(e, p->bo_list) {
		struct gsgpu_bo *bo = ttm_to_gsgpu_bo(e->tv.bo);
		bool userpage_invalidated = false;
		int i;

		e->user_pages = kvmalloc_array(bo->tbo.ttm->num_pages,
					       sizeof(struct page *),
					       GFP_KERNEL | __GFP_ZERO);
		if (!e->user_pages) {
			DRM_ERROR("calloc failure\n");
			return -ENOMEM;
		}

		r = gsgpu_ttm_tt_get_user_pages(bo, e->user_pages, &e->range);
		if (r) {
			kvfree(e->user_pages);
			e->user_pages = NULL;
			return r;
		}

		for (i = 0; i < bo->tbo.ttm->num_pages; i++) {
			if (bo->tbo.ttm->pages[i] != e->user_pages[i]) {
				userpage_invalidated = true;
				break;
			}
		}
		e->user_invalidated = userpage_invalidated;
	}

	r = ttm_eu_reserve_buffers(&p->ticket, &p->validated, true,
				   &duplicates);
	if (unlikely(r != 0)) {
		if (r != -ERESTARTSYS)
			DRM_ERROR("ttm_eu_reserve_buffers failed.\n");
		goto out;
	}

	gsgpu_cs_get_threshold_for_moves(p->adev, &p->bytes_moved_threshold,
					  &p->bytes_moved_vis_threshold);
	p->bytes_moved = 0;
	p->bytes_moved_vis = 0;
	p->evictable = list_last_entry(&p->validated,
				       struct gsgpu_bo_list_entry,
				       tv.head);

	r = gsgpu_vm_validate_pt_bos(p->adev, &fpriv->vm,
				      gsgpu_cs_validate, p);
	if (r) {
		DRM_ERROR("gsgpu_vm_validate_pt_bos() failed.\n");
		goto error_validate;
	}

	r = gsgpu_cs_list_validate(p, &duplicates);
	if (r) {
		DRM_ERROR("gsgpu_cs_list_validate(duplicates) failed.\n");
		goto error_validate;
	}

	r = gsgpu_cs_list_validate(p, &p->validated);
	if (r) {
		DRM_ERROR("gsgpu_cs_list_validate(validated) failed.\n");
		goto error_validate;
	}

	gsgpu_cs_report_moved_bytes(p->adev, p->bytes_moved,
				     p->bytes_moved_vis);

	gsgpu_bo_list_for_each_entry(e, p->bo_list)
		e->bo_va = gsgpu_vm_bo_find(vm, e->robj);

	if (!r && p->uf_entry.robj) {
		struct gsgpu_bo *uf = p->uf_entry.robj;

		r = gsgpu_ttm_alloc_gart(&uf->tbo);
		p->job->uf_addr += gsgpu_bo_gpu_offset(uf);
	}

error_validate:
	if (r)
		ttm_eu_backoff_reservation(&p->ticket, &p->validated);
out:
	return r;
}

static int gsgpu_cs_sync_rings(struct gsgpu_cs_parser *p)
{
	struct gsgpu_bo_list_entry *e;
	int r;

	list_for_each_entry(e, &p->validated, tv.head) {
		struct dma_resv *resv = e->robj->tbo.base.resv;
		r = gsgpu_sync_resv(p->adev, &p->job->sync, resv, p->filp,
				     gsgpu_bo_explicit_sync(e->robj));

		if (r)
			return r;
	}
	return 0;
}

/**
 * cs_parser_fini() - clean parser states
 * @parser:	parser structure holding parsing context.
 * @error:	error number
 *
 * If error is set than unvalidate buffer, otherwise just free memory
 * used by parsing context.
 **/
static void gsgpu_cs_parser_fini(struct gsgpu_cs_parser *parser, int error,
				  bool backoff)
{
	unsigned i;

	if (error && backoff)
		ttm_eu_backoff_reservation(&parser->ticket,
					   &parser->validated);

	for (i = 0; i < parser->num_post_dep_syncobjs; i++)
		drm_syncobj_put(parser->post_dep_syncobjs[i]);
	kfree(parser->post_dep_syncobjs);

	dma_fence_put(parser->fence);

	if (parser->ctx) {
		mutex_unlock(&parser->ctx->lock);
		gsgpu_ctx_put(parser->ctx);
	}
	if (parser->bo_list)
		gsgpu_bo_list_put(parser->bo_list);

	for (i = 0; i < parser->nchunks; i++)
		kvfree(parser->chunks[i].kdata);
	kfree(parser->chunks);
	if (parser->job)
		gsgpu_job_free(parser->job);
	gsgpu_bo_unref(&parser->uf_entry.robj);
}

static int gsgpu_bo_vm_update_pte(struct gsgpu_cs_parser *p)
{
	struct gsgpu_fpriv *fpriv = p->filp->driver_priv;
	struct gsgpu_device *adev = p->adev;
	struct gsgpu_vm *vm = &fpriv->vm;
	struct gsgpu_bo_list_entry *e;
	struct gsgpu_bo_va *bo_va;
	struct gsgpu_bo *bo;
	int r;

	r = gsgpu_vm_clear_freed(adev, vm, NULL);
	if (r)
		return r;

	r = gsgpu_vm_bo_update(adev, fpriv->prt_va, false);
	if (r)
		return r;

	r = gsgpu_sync_fence(adev, &p->job->sync,
			      fpriv->prt_va->last_pt_update, false);
	if (r)
		return r;

	gsgpu_bo_list_for_each_entry(e, p->bo_list) {
		struct dma_fence *f;

		/* ignore duplicates */
		bo = e->robj;
		if (!bo)
			continue;

		bo_va = e->bo_va;
		if (bo_va == NULL)
			continue;

		r = gsgpu_vm_bo_update(adev, bo_va, false);
		if (r)
			return r;

		f = bo_va->last_pt_update;
		r = gsgpu_sync_fence(adev, &p->job->sync, f, false);
		if (r)
			return r;
	}

	r = gsgpu_vm_handle_moved(adev, vm);
	if (r)
		return r;

	r = gsgpu_vm_update_directories(adev, vm);
	if (r)
		return r;

	r = gsgpu_sync_fence(adev, &p->job->sync, vm->last_update, false);
	if (r)
		return r;

	if (gsgpu_vm_debug) {
		/* Invalidate all BOs to test for userspace bugs */
		gsgpu_bo_list_for_each_entry(e, p->bo_list) {
			/* ignore duplicates */
			if (!e->robj)
				continue;

			gsgpu_vm_bo_invalidate(adev, e->robj, false);
		}
	}

	return r;
}

static int gsgpu_cs_ib_vm_chunk(struct gsgpu_device *adev,
				 struct gsgpu_cs_parser *p)
{
	struct gsgpu_fpriv *fpriv = p->filp->driver_priv;
	struct gsgpu_vm *vm = &fpriv->vm;
	int r;

	if (p->job->vm) {
		p->job->vm_pd_addr = gsgpu_bo_gpu_offset(vm->root.base.bo);

		r = gsgpu_bo_vm_update_pte(p);
		if (r)
			return r;

		r = dma_resv_reserve_fences(vm->root.base.bo->tbo.base.resv, 1);
		if (r)
			return r;
	}

	return gsgpu_cs_sync_rings(p);
}

static int gsgpu_cs_ib_fill(struct gsgpu_device *adev,
			     struct gsgpu_cs_parser *parser)
{
	struct gsgpu_fpriv *fpriv = parser->filp->driver_priv;
	struct gsgpu_vm *vm = &fpriv->vm;
	int i, j;
	int r;

	for (i = 0, j = 0; i < parser->nchunks && j < parser->job->num_ibs; i++) {
		struct gsgpu_cs_chunk *chunk;
		struct gsgpu_ib *ib;
		struct drm_gsgpu_cs_chunk_ib *chunk_ib;
		struct gsgpu_ring *ring;

		chunk = &parser->chunks[i];
		ib = &parser->job->ibs[j];
		chunk_ib = (struct drm_gsgpu_cs_chunk_ib *)chunk->kdata;

		if (chunk->chunk_id != GSGPU_CHUNK_ID_IB)
			continue;

		r = gsgpu_queue_mgr_map(adev, &parser->ctx->queue_mgr, chunk_ib->ip_type,
					 chunk_ib->ip_instance, chunk_ib->ring, &ring);
		if (r)
			return r;

		if (chunk_ib->flags & GSGPU_IB_FLAG_PREAMBLE)
			parser->job->preamble_status |=
				GSGPU_PREAMBLE_IB_PRESENT;

		if (parser->ring && parser->ring != ring)
			return -EINVAL;

		parser->ring = ring;

		r =  gsgpu_ib_get(adev, vm,
					ring->funcs->parse_cs ? chunk_ib->ib_bytes : 0,
					ib);
		if (r) {
			DRM_ERROR("Failed to get ib !\n");
			return r;
		}

		ib->gpu_addr = chunk_ib->va_start;
		ib->length_dw = chunk_ib->ib_bytes / 4;
		ib->flags = chunk_ib->flags;

		j++;
	}

	return gsgpu_ctx_wait_prev_fence(parser->ctx, parser->ring->idx);
}

static int gsgpu_cs_process_fence_dep(struct gsgpu_cs_parser *p,
				       struct gsgpu_cs_chunk *chunk)
{
	struct gsgpu_fpriv *fpriv = p->filp->driver_priv;
	unsigned num_deps;
	int i, r;
	struct drm_gsgpu_cs_chunk_dep *deps;

	deps = (struct drm_gsgpu_cs_chunk_dep *)chunk->kdata;
	num_deps = chunk->length_dw * 4 /
		sizeof(struct drm_gsgpu_cs_chunk_dep);

	for (i = 0; i < num_deps; ++i) {
		struct gsgpu_ring *ring;
		struct gsgpu_ctx *ctx;
		struct dma_fence *fence;

		ctx = gsgpu_ctx_get(fpriv, deps[i].ctx_id);
		if (ctx == NULL)
			return -EINVAL;

		r = gsgpu_queue_mgr_map(p->adev, &ctx->queue_mgr,
					 deps[i].ip_type,
					 deps[i].ip_instance,
					 deps[i].ring, &ring);
		if (r) {
			gsgpu_ctx_put(ctx);
			return r;
		}

		fence = gsgpu_ctx_get_fence(ctx, ring,
					     deps[i].handle);
		if (IS_ERR(fence)) {
			r = PTR_ERR(fence);
			gsgpu_ctx_put(ctx);
			return r;
		} else if (fence) {
			r = gsgpu_sync_fence(p->adev, &p->job->sync, fence,
					true);
			dma_fence_put(fence);
			gsgpu_ctx_put(ctx);
			if (r)
				return r;
		}
	}
	return 0;
}

static int gsgpu_syncobj_lookup_and_add_to_sync(struct gsgpu_cs_parser *p,
						 uint32_t handle)
{
	int r;
	struct dma_fence *fence;
	r = drm_syncobj_find_fence(p->filp, handle, 0, 0, &fence);
	if (r)
		return r;

	r = gsgpu_sync_fence(p->adev, &p->job->sync, fence, true);
	dma_fence_put(fence);

	return r;
}

static int gsgpu_cs_process_syncobj_in_dep(struct gsgpu_cs_parser *p,
					    struct gsgpu_cs_chunk *chunk)
{
	unsigned num_deps;
	int i, r;
	struct drm_gsgpu_cs_chunk_sem *deps;

	deps = (struct drm_gsgpu_cs_chunk_sem *)chunk->kdata;
	num_deps = chunk->length_dw * 4 /
		sizeof(struct drm_gsgpu_cs_chunk_sem);

	for (i = 0; i < num_deps; ++i) {
		r = gsgpu_syncobj_lookup_and_add_to_sync(p, deps[i].handle);
		if (r)
			return r;
	}
	return 0;
}

static int gsgpu_cs_process_syncobj_out_dep(struct gsgpu_cs_parser *p,
					     struct gsgpu_cs_chunk *chunk)
{
	unsigned num_deps;
	int i;
	struct drm_gsgpu_cs_chunk_sem *deps;
	deps = (struct drm_gsgpu_cs_chunk_sem *)chunk->kdata;
	num_deps = chunk->length_dw * 4 /
		sizeof(struct drm_gsgpu_cs_chunk_sem);

	p->post_dep_syncobjs = kmalloc_array(num_deps,
					     sizeof(struct drm_syncobj *),
					     GFP_KERNEL);
	p->num_post_dep_syncobjs = 0;

	if (!p->post_dep_syncobjs)
		return -ENOMEM;

	for (i = 0; i < num_deps; ++i) {
		p->post_dep_syncobjs[i] = drm_syncobj_find(p->filp, deps[i].handle);
		if (!p->post_dep_syncobjs[i])
			return -EINVAL;
		p->num_post_dep_syncobjs++;
	}
	return 0;
}

static int gsgpu_cs_dependencies(struct gsgpu_device *adev,
				  struct gsgpu_cs_parser *p)
{
	int i, r;

	for (i = 0; i < p->nchunks; ++i) {
		struct gsgpu_cs_chunk *chunk;

		chunk = &p->chunks[i];

		if (chunk->chunk_id == GSGPU_CHUNK_ID_DEPENDENCIES) {
			r = gsgpu_cs_process_fence_dep(p, chunk);
			if (r)
				return r;
		} else if (chunk->chunk_id == GSGPU_CHUNK_ID_SYNCOBJ_IN) {
			r = gsgpu_cs_process_syncobj_in_dep(p, chunk);
			if (r)
				return r;
		} else if (chunk->chunk_id == GSGPU_CHUNK_ID_SYNCOBJ_OUT) {
			r = gsgpu_cs_process_syncobj_out_dep(p, chunk);
			if (r)
				return r;
		}
	}

	return 0;
}

static void gsgpu_cs_post_dependencies(struct gsgpu_cs_parser *p)
{
	int i;

	for (i = 0; i < p->num_post_dep_syncobjs; ++i)
		drm_syncobj_replace_fence(p->post_dep_syncobjs[i], p->fence);
}

static int gsgpu_cs_submit(struct gsgpu_cs_parser *p,
			    union drm_gsgpu_cs *cs)
{
	struct gsgpu_fpriv *fpriv = p->filp->driver_priv;
	struct gsgpu_ring *ring = p->ring;
	struct drm_sched_entity *entity = &p->ctx->rings[ring->idx].entity;
	enum drm_sched_priority priority;
	struct gsgpu_bo_list_entry *e;
	struct gsgpu_job *job;
	uint64_t seq;
	int r;
	job = p->job;
	p->job = NULL;

	r = drm_sched_job_init(&job->base, entity, p->filp);
	if (r)
		goto error_unlock;

	drm_sched_job_arm(&job->base);

	/* No memory allocation is allowed while holding the notifier lock.
	 * The lock is held until gsgpu_cs_submit is finished and fence is
	 * added to BOs.
         */
	mutex_lock(&p->adev->notifier_lock);

	/* If userptr are invalidated after gsgpu_cs_parser_bos(), return
	 * -EAGAIN, drmIoctl in libdrm will restart the gsgpu_cs_ioctl.
	 */
	gsgpu_bo_list_for_each_userptr_entry(e, p->bo_list) {
		struct gsgpu_bo *bo = e->robj;
		r |= !gsgpu_ttm_tt_get_user_pages_done(bo->tbo.ttm, e->range);
	}
	if (r) {
		r = -EAGAIN;
		goto error_abort;
	}

	job->owner = p->filp;
	p->fence = dma_fence_get(&job->base.s_fence->finished);

	r = gsgpu_ctx_add_fence(p->ctx, ring, p->fence, &seq);
	if (r) {
		dma_fence_put(p->fence);
		dma_fence_put(&job->base.s_fence->finished);
		gsgpu_job_free(job);
		mutex_unlock(&p->adev->notifier_lock);
		return r;
	}

	gsgpu_cs_post_dependencies(p);

	if ((job->preamble_status & GSGPU_PREAMBLE_IB_PRESENT) &&
	    !p->ctx->preamble_presented) {
		job->preamble_status |= GSGPU_PREAMBLE_IB_PRESENT_FIRST;
		p->ctx->preamble_presented = true;
	}

	cs->out.handle = seq;
	job->uf_sequence = seq;

	gsgpu_job_free_resources(job);

	trace_gsgpu_cs_ioctl(job);
	gsgpu_vm_bo_trace_cs(&fpriv->vm, &p->ticket);
	priority = job->base.s_priority;
	drm_sched_entity_push_job(&job->base);

	ring = to_gsgpu_ring(entity->rq->sched);
	gsgpu_ring_priority_get(ring, priority);

	/* Make sure all BOs are remembered as writers */
	gsgpu_bo_list_for_each_entry(e, p->bo_list) {
		e->tv.num_shared = 0;
	}

	ttm_eu_fence_buffer_objects(&p->ticket, &p->validated, p->fence);
	mutex_unlock(&p->adev->notifier_lock);

	return 0;

error_abort:
	dma_fence_put(&job->base.s_fence->finished);
	job->base.s_fence = NULL;
	mutex_unlock(&p->adev->notifier_lock);

error_unlock:
	gsgpu_job_free(job);
	return r;
}

int gsgpu_cs_ioctl(struct drm_device *dev, void *data, struct drm_file *filp)
{
	struct gsgpu_device *adev = dev->dev_private;
	union drm_gsgpu_cs *cs = data;
	struct gsgpu_cs_parser parser = {};
	bool reserved_buffers = false;
	int i, r;

	if (!adev->accel_working)
		return -EBUSY;

	parser.adev = adev;
	parser.filp = filp;

	r = gsgpu_cs_parser_init(&parser, data);
	if (r) {
		DRM_ERROR("Failed to initialize parser !\n");
		goto out;
	}

	r = gsgpu_cs_ib_fill(adev, &parser);
	if (r)
		goto out;

	r = gsgpu_cs_parser_bos(&parser, data);
	if (r) {
		if (r == -ENOMEM)
			DRM_ERROR("Not enough memory for command submission!\n");
		else if (r != -ERESTARTSYS)
			DRM_ERROR("Failed to process the buffer list %d!\n", r);
		goto out;
	}

	reserved_buffers = true;

	r = gsgpu_cs_dependencies(adev, &parser);
	if (r) {
		DRM_ERROR("Failed in the dependencies handling %d!\n", r);
		goto out;
	}

	for (i = 0; i < parser.job->num_ibs; i++)
		trace_gsgpu_cs(&parser, i);

	r = gsgpu_cs_ib_vm_chunk(adev, &parser);
	if (r)
		goto out;

	r = gsgpu_cs_submit(&parser, cs);

out:
	gsgpu_cs_parser_fini(&parser, r, reserved_buffers);
	return r;
}

/**
 * gsgpu_cs_wait_ioctl - wait for a command submission to finish
 *
 * @dev: drm device
 * @data: data from userspace
 * @filp: file private
 *
 * Wait for the command submission identified by handle to finish.
 */
int gsgpu_cs_wait_ioctl(struct drm_device *dev, void *data,
			 struct drm_file *filp)
{
	union drm_gsgpu_wait_cs *wait = data;
	struct gsgpu_device *adev = dev->dev_private;
	unsigned long timeout = gsgpu_gem_timeout(wait->in.timeout);
	struct gsgpu_ring *ring = NULL;
	struct gsgpu_ctx *ctx;
	struct dma_fence *fence;
	long r;

	ctx = gsgpu_ctx_get(filp->driver_priv, wait->in.ctx_id);
	if (ctx == NULL)
		return -EINVAL;

	r = gsgpu_queue_mgr_map(adev, &ctx->queue_mgr,
				 wait->in.ip_type, wait->in.ip_instance,
				 wait->in.ring, &ring);
	if (r) {
		gsgpu_ctx_put(ctx);
		return r;
	}

	fence = gsgpu_ctx_get_fence(ctx, ring, wait->in.handle);
	if (IS_ERR(fence))
		r = PTR_ERR(fence);
	else if (fence) {
		r = dma_fence_wait_timeout(fence, true, timeout);
		if (r > 0 && fence->error)
			r = fence->error;
		dma_fence_put(fence);
	} else
		r = 1;

	gsgpu_ctx_put(ctx);
	if (r < 0)
		return r;

	memset(wait, 0, sizeof(*wait));
	wait->out.status = (r == 0);

	return 0;
}

/**
 * gsgpu_cs_get_fence - helper to get fence from drm_gsgpu_fence
 *
 * @adev: gsgpu device
 * @filp: file private
 * @user: drm_gsgpu_fence copied from user space
 */
static struct dma_fence *gsgpu_cs_get_fence(struct gsgpu_device *adev,
					     struct drm_file *filp,
					     struct drm_gsgpu_fence *user)
{
	struct gsgpu_ring *ring;
	struct gsgpu_ctx *ctx;
	struct dma_fence *fence;
	int r;

	ctx = gsgpu_ctx_get(filp->driver_priv, user->ctx_id);
	if (ctx == NULL)
		return ERR_PTR(-EINVAL);

	r = gsgpu_queue_mgr_map(adev, &ctx->queue_mgr, user->ip_type,
				 user->ip_instance, user->ring, &ring);
	if (r) {
		gsgpu_ctx_put(ctx);
		return ERR_PTR(r);
	}

	fence = gsgpu_ctx_get_fence(ctx, ring, user->seq_no);
	gsgpu_ctx_put(ctx);

	return fence;
}

int gsgpu_cs_fence_to_handle_ioctl(struct drm_device *dev, void *data,
				    struct drm_file *filp)
{
	struct gsgpu_device *adev = dev->dev_private;
	union drm_gsgpu_fence_to_handle *info = data;
	struct dma_fence *fence;
	struct drm_syncobj *syncobj;
	struct sync_file *sync_file;
	int fd, r;

	fence = gsgpu_cs_get_fence(adev, filp, &info->in.fence);
	if (IS_ERR(fence))
		return PTR_ERR(fence);

	switch (info->in.what) {
	case GSGPU_FENCE_TO_HANDLE_GET_SYNCOBJ:
		r = drm_syncobj_create(&syncobj, 0, fence);
		dma_fence_put(fence);
		if (r)
			return r;
		r = drm_syncobj_get_handle(filp, syncobj, &info->out.handle);
		drm_syncobj_put(syncobj);
		return r;

	case GSGPU_FENCE_TO_HANDLE_GET_SYNCOBJ_FD:
		r = drm_syncobj_create(&syncobj, 0, fence);
		dma_fence_put(fence);
		if (r)
			return r;
		r = drm_syncobj_get_fd(syncobj, (int *)&info->out.handle);
		drm_syncobj_put(syncobj);
		return r;

	case GSGPU_FENCE_TO_HANDLE_GET_SYNC_FILE_FD:
		fd = get_unused_fd_flags(O_CLOEXEC);
		if (fd < 0) {
			dma_fence_put(fence);
			return fd;
		}

		sync_file = sync_file_create(fence);
		dma_fence_put(fence);
		if (!sync_file) {
			put_unused_fd(fd);
			return -ENOMEM;
		}

		fd_install(fd, sync_file->file);
		info->out.handle = fd;
		return 0;

	default:
		return -EINVAL;
	}
}

/**
 * gsgpu_cs_wait_all_fence - wait on all fences to signal
 *
 * @adev: gsgpu device
 * @filp: file private
 * @wait: wait parameters
 * @fences: array of drm_gsgpu_fence
 */
static int gsgpu_cs_wait_all_fences(struct gsgpu_device *adev,
				     struct drm_file *filp,
				     union drm_gsgpu_wait_fences *wait,
				     struct drm_gsgpu_fence *fences)
{
	uint32_t fence_count = wait->in.fence_count;
	unsigned int i;
	long r = 1;

	for (i = 0; i < fence_count; i++) {
		struct dma_fence *fence;
		unsigned long timeout = gsgpu_gem_timeout(wait->in.timeout_ns);

		fence = gsgpu_cs_get_fence(adev, filp, &fences[i]);
		if (IS_ERR(fence))
			return PTR_ERR(fence);
		else if (!fence)
			continue;

		r = dma_fence_wait_timeout(fence, true, timeout);
		dma_fence_put(fence);
		if (r < 0)
			return r;

		if (r == 0)
			break;

		if (fence->error)
			return fence->error;
	}

	memset(wait, 0, sizeof(*wait));
	wait->out.status = (r > 0);

	return 0;
}

/**
 * gsgpu_cs_wait_any_fence - wait on any fence to signal
 *
 * @adev: gsgpu device
 * @filp: file private
 * @wait: wait parameters
 * @fences: array of drm_gsgpu_fence
 */
static int gsgpu_cs_wait_any_fence(struct gsgpu_device *adev,
				    struct drm_file *filp,
				    union drm_gsgpu_wait_fences *wait,
				    struct drm_gsgpu_fence *fences)
{
	unsigned long timeout = gsgpu_gem_timeout(wait->in.timeout_ns);
	uint32_t fence_count = wait->in.fence_count;
	uint32_t first = ~0;
	struct dma_fence **array;
	unsigned int i;
	long r;

	/* Prepare the fence array */
	array = kcalloc(fence_count, sizeof(struct dma_fence *), GFP_KERNEL);

	if (array == NULL)
		return -ENOMEM;

	for (i = 0; i < fence_count; i++) {
		struct dma_fence *fence;

		fence = gsgpu_cs_get_fence(adev, filp, &fences[i]);
		if (IS_ERR(fence)) {
			r = PTR_ERR(fence);
			goto err_free_fence_array;
		} else if (fence) {
			array[i] = fence;
		} else { /* NULL, the fence has been already signaled */
			r = 1;
			first = i;
			goto out;
		}
	}

	r = dma_fence_wait_any_timeout(array, fence_count, true, timeout,
				       &first);
	if (r < 0)
		goto err_free_fence_array;

out:
	memset(wait, 0, sizeof(*wait));
	wait->out.status = (r > 0);
	wait->out.first_signaled = first;

	if (first < fence_count && array[first])
		r = array[first]->error;
	else
		r = 0;

err_free_fence_array:
	for (i = 0; i < fence_count; i++)
		dma_fence_put(array[i]);
	kfree(array);

	return r;
}

/**
 * gsgpu_cs_wait_fences_ioctl - wait for multiple command submissions to finish
 *
 * @dev: drm device
 * @data: data from userspace
 * @filp: file private
 */
int gsgpu_cs_wait_fences_ioctl(struct drm_device *dev, void *data,
				struct drm_file *filp)
{
	struct gsgpu_device *adev = dev->dev_private;
	union drm_gsgpu_wait_fences *wait = data;
	uint32_t fence_count = wait->in.fence_count;
	struct drm_gsgpu_fence *fences_user;
	struct drm_gsgpu_fence *fences;
	int r;

	/* Get the fences from userspace */
	fences = kmalloc_array(fence_count, sizeof(struct drm_gsgpu_fence),
			GFP_KERNEL);
	if (fences == NULL)
		return -ENOMEM;

	fences_user = u64_to_user_ptr(wait->in.fences);
	if (copy_from_user(fences, fences_user,
		sizeof(struct drm_gsgpu_fence) * fence_count)) {
		r = -EFAULT;
		goto err_free_fences;
	}

	if (wait->in.wait_all)
		r = gsgpu_cs_wait_all_fences(adev, filp, wait, fences);
	else
		r = gsgpu_cs_wait_any_fence(adev, filp, wait, fences);

err_free_fences:
	kfree(fences);

	return r;
}

/**
 * gsgpu_cs_find_bo_va - find bo_va for VM address
 *
 * @parser: command submission parser context
 * @addr: VM address
 * @bo: resulting BO of the mapping found
 *
 * Search the buffer objects in the command submission context for a certain
 * virtual memory address. Returns allocation structure when found, NULL
 * otherwise.
 */
int gsgpu_cs_find_mapping(struct gsgpu_cs_parser *parser,
			   uint64_t addr, struct gsgpu_bo **bo,
			   struct gsgpu_bo_va_mapping **map)
{
	struct gsgpu_fpriv *fpriv = parser->filp->driver_priv;
	struct ttm_operation_ctx ctx = { false, false };
	struct gsgpu_vm *vm = &fpriv->vm;
	struct gsgpu_bo_va_mapping *mapping;
	int r;

	addr /= GSGPU_GPU_PAGE_SIZE;

	mapping = gsgpu_vm_bo_lookup_mapping(vm, addr);
	if (!mapping || !mapping->bo_va || !mapping->bo_va->base.bo)
		return -EINVAL;

	*bo = mapping->bo_va->base.bo;
	*map = mapping;

	/* Double check that the BO is reserved by this CS */
	if (READ_ONCE((*bo)->tbo.base.resv->lock.ctx) != &parser->ticket)
		return -EINVAL;

	if (!((*bo)->flags & GSGPU_GEM_CREATE_VRAM_CONTIGUOUS)) {
		(*bo)->flags |= GSGPU_GEM_CREATE_VRAM_CONTIGUOUS;
		gsgpu_bo_placement_from_domain(*bo, (*bo)->allowed_domains);
		r = ttm_bo_validate(&(*bo)->tbo, &(*bo)->placement, &ctx);
		if (r)
			return r;
	}

	return gsgpu_ttm_alloc_gart(&(*bo)->tbo);
}
