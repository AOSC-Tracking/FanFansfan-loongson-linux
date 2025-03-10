#include <drm/gsgpu_drm.h>
#include "gsgpu.h"

/* Test BO GTT->VRAM and VRAM->GTT GPU copies across the whole GTT aperture */
static void gsgpu_do_test_moves(struct gsgpu_device *adev)
{
	struct gsgpu_ring *ring = adev->mman.buffer_funcs_ring;
	struct gsgpu_bo *vram_obj = NULL;
	struct gsgpu_bo **gtt_obj = NULL;
	struct gsgpu_bo_param bp;
	uint64_t gart_addr, vram_addr;
	unsigned n, size;
	int i, r;

	size = 1024 * 1024;

	/* Number of tests =
	 * (Total GTT - IB pool - writeback page - ring buffers) / test size
	 */
	n = adev->gmc.gart_size - GSGPU_IB_POOL_SIZE*64*1024;
	for (i = 0; i < GSGPU_MAX_RINGS; ++i)
		if (adev->rings[i])
			n -= adev->rings[i]->ring_size;
	if (adev->wb.wb_obj)
		n -= GSGPU_GPU_PAGE_SIZE;
	if (adev->irq.ih.ring_obj)
		n -= adev->irq.ih.ring_size;
	n /= size;

	gtt_obj = kcalloc(n, sizeof(*gtt_obj), GFP_KERNEL);
	if (!gtt_obj) {
		DRM_ERROR("Failed to allocate %d pointers\n", n);
		r = 1;
		goto out_cleanup;
	}
	memset(&bp, 0, sizeof(bp));
	bp.size = size;
	bp.byte_align = PAGE_SIZE;
	bp.domain = GSGPU_GEM_DOMAIN_VRAM;
	bp.flags = 0;
	bp.type = ttm_bo_type_kernel;
	bp.resv = NULL;

	r = gsgpu_bo_create(adev, &bp, &vram_obj);
	if (r) {
		DRM_ERROR("Failed to create VRAM object\n");
		goto out_cleanup;
	}
	r = gsgpu_bo_reserve(vram_obj, false);
	if (unlikely(r != 0))
		goto out_unref;
	r = gsgpu_bo_pin(vram_obj, GSGPU_GEM_DOMAIN_VRAM);
	if (r) {
		DRM_ERROR("Failed to pin VRAM object\n");
		goto out_unres;
	}
	vram_addr = gsgpu_bo_gpu_offset(vram_obj);
	for (i = 0; i < n; i++) {
		void *gtt_map, *vram_map;
		void **gart_start, **gart_end;
		void **vram_start, **vram_end;
		struct dma_fence *fence = NULL;

		bp.domain = GSGPU_GEM_DOMAIN_GTT;
		r = gsgpu_bo_create(adev, &bp, gtt_obj + i);
		if (r) {
			DRM_ERROR("Failed to create GTT object %d\n", i);
			goto out_lclean;
		}

		r = gsgpu_bo_reserve(gtt_obj[i], false);
		if (unlikely(r != 0))
			goto out_lclean_unref;
		r = gsgpu_bo_pin(gtt_obj[i], GSGPU_GEM_DOMAIN_GTT);
		if (r) {
			DRM_ERROR("Failed to pin GTT object %d\n", i);
			goto out_lclean_unres;
		}
		r = gsgpu_ttm_alloc_gart(&gtt_obj[i]->tbo);
		if (r) {
			DRM_ERROR("0x%px bind failed\n", gtt_obj[i]);
			goto out_lclean_unpin;
		}
		gart_addr = gsgpu_bo_gpu_offset(gtt_obj[i]);

		r = gsgpu_bo_kmap(gtt_obj[i], &gtt_map);
		if (r) {
			DRM_ERROR("Failed to map GTT object %d\n", i);
			goto out_lclean_unpin;
		}

		for (gart_start = gtt_map, gart_end = gtt_map + size;
		     gart_start < gart_end;
		     gart_start++)
			*gart_start = gart_start;

		gsgpu_bo_kunmap(gtt_obj[i]);

		r = gsgpu_copy_buffer(ring, gart_addr, vram_addr,
				       size, NULL, &fence, false, false);

		if (r) {
			DRM_ERROR("Failed GTT->VRAM copy %d\n", i);
			goto out_lclean_unpin;
		}

		r = dma_fence_wait(fence, false);
		if (r) {
			DRM_ERROR("Failed to wait for GTT->VRAM fence %d\n", i);
			goto out_lclean_unpin;
		}

		dma_fence_put(fence);

		r = gsgpu_bo_kmap(vram_obj, &vram_map);
		if (r) {
			DRM_ERROR("Failed to map VRAM object after copy %d\n", i);
			goto out_lclean_unpin;
		}

		for (gart_start = gtt_map, gart_end = gtt_map + size,
		     vram_start = vram_map, vram_end = vram_map + size;
		     vram_start < vram_end;
		     gart_start++, vram_start++) {
			if (*vram_start != gart_start) {
				DRM_ERROR("Incorrect GTT->VRAM copy %d: Got 0x0x%px, "
					  "expected 0x0x%px (GTT/VRAM offset "
					  "0x%16llx/0x%16llx)\n",
					  i, *vram_start, gart_start,
					  (unsigned long long)
					  (gart_addr - adev->gmc.gart_start +
					   (void *)gart_start - gtt_map),
					  (unsigned long long)
					  (vram_addr - adev->gmc.vram_start +
					   (void *)gart_start - gtt_map));
				gsgpu_bo_kunmap(vram_obj);
				goto out_lclean_unpin;
			}
			*vram_start = vram_start;
		}

		gsgpu_bo_kunmap(vram_obj);

		r = gsgpu_copy_buffer(ring, vram_addr, gart_addr,
				       size, NULL, &fence, false, false);

		if (r) {
			DRM_ERROR("Failed VRAM->GTT copy %d\n", i);
			goto out_lclean_unpin;
		}

		r = dma_fence_wait(fence, false);
		if (r) {
			DRM_ERROR("Failed to wait for VRAM->GTT fence %d\n", i);
			goto out_lclean_unpin;
		}

		dma_fence_put(fence);

		r = gsgpu_bo_kmap(gtt_obj[i], &gtt_map);
		if (r) {
			DRM_ERROR("Failed to map GTT object after copy %d\n", i);
			goto out_lclean_unpin;
		}

		for (gart_start = gtt_map, gart_end = gtt_map + size,
		     vram_start = vram_map, vram_end = vram_map + size;
		     gart_start < gart_end;
		     gart_start++, vram_start++) {
			if (*gart_start != vram_start) {
				DRM_ERROR("Incorrect VRAM->GTT copy %d: Got 0x0x%px, "
					  "expected 0x0x%px (VRAM/GTT offset "
					  "0x%16llx/0x%16llx)\n",
					  i, *gart_start, vram_start,
					  (unsigned long long)
					  (vram_addr - adev->gmc.vram_start +
					   (void *)vram_start - vram_map),
					  (unsigned long long)
					  (gart_addr - adev->gmc.gart_start +
					   (void *)vram_start - vram_map));
				gsgpu_bo_kunmap(gtt_obj[i]);
				goto out_lclean_unpin;
			}
		}

		gsgpu_bo_kunmap(gtt_obj[i]);

		DRM_INFO("Tested GTT->VRAM and VRAM->GTT copy for GTT offset 0x%llx\n",
			 gart_addr - adev->gmc.gart_start);
		continue;

out_lclean_unpin:
		gsgpu_bo_unpin(gtt_obj[i]);
out_lclean_unres:
		gsgpu_bo_unreserve(gtt_obj[i]);
out_lclean_unref:
		gsgpu_bo_unref(&gtt_obj[i]);
out_lclean:
		for (--i; i >= 0; --i) {
			gsgpu_bo_unpin(gtt_obj[i]);
			gsgpu_bo_unreserve(gtt_obj[i]);
			gsgpu_bo_unref(&gtt_obj[i]);
		}
		if (fence)
			dma_fence_put(fence);
		break;
	}

	gsgpu_bo_unpin(vram_obj);
out_unres:
	gsgpu_bo_unreserve(vram_obj);
out_unref:
	gsgpu_bo_unref(&vram_obj);
out_cleanup:
	kfree(gtt_obj);
	if (r) {
		pr_warn("Error while testing BO move\n");
	}
}

void gsgpu_test_moves(struct gsgpu_device *adev)
{
	if (adev->mman.buffer_funcs)
		gsgpu_do_test_moves(adev);
}
