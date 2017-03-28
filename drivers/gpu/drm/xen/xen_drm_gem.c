/*
 *  Xen para-virtual DRM device
 *
 *   This program is free software; you can redistribute it and/or modify
 *   it under the terms of the GNU General Public License as published by
 *   the Free Software Foundation; either version 2 of the License, or
 *   (at your option) any later version.
 *
 *   This program is distributed in the hope that it will be useful,
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *   GNU General Public License for more details.
 *
 * Copyright (C) 2016 EPAM Systems Inc.
 *
 * Author: Oleksandr Andrushchenko <Oleksandr_Andrushchenko@epam.com>
 */

#include <drm/drmP.h>
#include <drm/drm_crtc_helper.h>
#include <drm/drm_fb_helper.h>
#include <drm/drm_gem.h>

#include <linux/dma-buf.h>
#include <linux/scatterlist.h>

#include "xen_drm.h"
#include "xen_drm_gem.h"

struct xen_gem_object {
	struct drm_gem_object base;
	size_t size;
	/* this is set if we have allocated buffer on our own */
	struct sg_table *sgt;
	/* this is for external buffer allocated by back */
	struct sg_table *sgt_ext;
	/* this is for imported buffer */
	struct sg_table *sgt_imported;
};

struct xen_fb {
	struct drm_framebuffer fb;
	struct xen_gem_object *xen_obj;
};

static inline struct xen_gem_object *to_xen_gem_obj(
	struct drm_gem_object *gem_obj)
{
	return container_of(gem_obj, struct xen_gem_object, base);
}

static inline struct xen_fb *to_xen_fb(struct drm_framebuffer *fb)
{
	return container_of(fb, struct xen_fb, fb);
}

static void xendrm_gem_dump(const char *fn, struct xen_gem_object *xen_obj)
{
	DRM_ERROR("%s xen_obj->sgt %p xen_obj->sgt_imported %p xen_obj->sgt_ext %p\n",
			fn, xen_obj->sgt, xen_obj->sgt_imported, xen_obj->sgt_ext);
	DRM_ERROR("%s xen_obj->base.import_attach %p\n", fn, xen_obj->base.import_attach);
}

static void xendrm_gem_dump_sgt(const char *fn, const char *sgt_name,
	struct sg_table *sgt)
{
	struct scatterlist *sg;
	int i;

	DRM_ERROR("%s sgt is %s\n", fn, sgt_name);
	for_each_sg(sgt->sgl, sg, sgt->nents, i) {
		DRM_ERROR("vaddr %p length %u\n", sg_page(sg), sg->length);
	}
}

static struct sg_table *xendrm_gem_alloc(size_t size)
{
	struct sg_table *sgt;
	struct scatterlist *sg;
	struct chunk {
		void *vaddr;
		size_t size;
	} *chunks;
	size_t need_sz, chunk_sz, num_chunks;
	unsigned int chunk_order;
	int ret, i;

	BUG_ON(size % PAGE_SIZE);
	/* FIXME: we don't know how many chunks will be there,
	 * so cannot allocate sg table now. if we are not lucky
	 * we'll end up with single pages for all the requested buffer
	 */
	chunks = drm_malloc_ab(size / PAGE_SIZE, sizeof(*chunks));
	if (!chunks)
		return NULL;
	need_sz = size;
	chunk_sz = size;
	num_chunks = 0;
	do {
		struct page *page;

		chunk_order = get_order(chunk_sz);
		if (likely(chunk_order < MAX_ORDER)) {
			if (need_sz < (1 << chunk_order) * PAGE_SIZE)
				chunk_order--;
		} else {
			chunk_order = MAX_ORDER - 1;
		}
		chunk_sz = (1 << chunk_order) * PAGE_SIZE;
		page = alloc_pages(GFP_KERNEL | __GFP_ZERO, chunk_order);
		if (page) {
			chunks[num_chunks].vaddr = page_to_virt(page);
			DRM_ERROR("%s alloc_pages %p\n", __FUNCTION__, chunks[num_chunks].vaddr);
			chunks[num_chunks++].size = chunk_sz;
			need_sz -= chunk_sz;
			chunk_sz = need_sz;
			continue;
		}
		if (unlikely(chunk_sz == PAGE_SIZE))
			goto fail_nomem;
	} while (need_sz);
	sgt = kmalloc(sizeof(*sgt), GFP_KERNEL);
	if (!sgt)
		goto fail_nomem;
	ret = sg_alloc_table(sgt, num_chunks, GFP_KERNEL);
	if (ret < 0)
		goto fail_sgt;
	for_each_sg(sgt->sgl, sg, num_chunks, i)
		sg_set_buf(sg, chunks[i].vaddr, chunks[i].size);
	drm_free_large(chunks);
	return sgt;

fail_sgt:
	kfree(sgt);
fail_nomem:
	for (i = 0; i < num_chunks; i++)
		free_pages((unsigned long)chunks[i].vaddr,
			get_order(chunks[i].size));
	drm_free_large(chunks);
	return NULL;
}

static void xendrm_gem_free(struct xen_gem_object *xen_obj)
{
	xendrm_gem_dump(__FUNCTION__, xen_obj);
	if (xen_obj->base.import_attach) {
		DRM_ERROR("%s drm_prime_gem_destroy\n", __FUNCTION__);
		drm_prime_gem_destroy(&xen_obj->base, xen_obj->sgt_imported);
	}
	if (xen_obj->sgt) {
		struct scatterlist *sg;
		int i;

		xendrm_gem_dump_sgt(__FUNCTION__, "xen_obj->sgt", xen_obj->sgt);
		for_each_sg(xen_obj->sgt->sgl, sg, xen_obj->sgt->nents, i) {
			DRM_ERROR("%s free at %p size %u\n",
				__FUNCTION__, sg_virt(sg), sg->length);
			free_pages((unsigned long)sg_virt(sg),
				get_order(sg->length));
		}
		sg_free_table(xen_obj->sgt);
		kfree(xen_obj->sgt);
	}
	if (xen_obj->sgt_ext) {
		xendrm_gem_dump_sgt(__FUNCTION__, "xen_obj->sgt_ext", xen_obj->sgt_ext);
		sg_free_table(xen_obj->sgt_ext);
		kfree(xen_obj->sgt_ext);
	}
}

static struct xen_gem_object *xendrm_gem_create_obj(struct drm_device *dev,
	size_t size)
{
	struct xen_gem_object *xen_obj;
	struct drm_gem_object *gem_obj;
	int ret;

	xen_obj = kzalloc(sizeof(*xen_obj), GFP_KERNEL);
	if (!xen_obj)
		return ERR_PTR(-ENOMEM);
	gem_obj = &xen_obj->base;
	ret = drm_gem_object_init(dev, gem_obj, size);
	if (ret)
		goto error;
	ret = drm_gem_create_mmap_offset(gem_obj);
	if (ret) {
		drm_gem_object_release(gem_obj);
		goto error;
	}
	return xen_obj;

error:
	kfree(xen_obj);
	return ERR_PTR(ret);
}

static struct xen_gem_object *xendrm_gem_create(struct drm_device *dev,
	size_t size)
{
	struct xendrm_device *xendrm_dev = dev->dev_private;
	struct xen_gem_object *xen_obj;
	int ret;

	DRM_ERROR("%s size %zu\n", __FUNCTION__, size);
	size = round_up(size, PAGE_SIZE);
	xen_obj = xendrm_gem_create_obj(dev, size);
	if (IS_ERR(xen_obj))
		return xen_obj;
	xen_obj->size = size;
	if (xendrm_dev->platdata->ext_buffers)
		return xen_obj;
	xen_obj->sgt = xendrm_gem_alloc(size);
	xendrm_gem_dump_sgt(__FUNCTION__, "xen_obj->sgt", xen_obj->sgt);
	if (!xen_obj->sgt) {
		ret = -ENOMEM;
		goto fail;
	}
	return xen_obj;

fail:
	DRM_ERROR("Failed to allocate buffer with size %zu\n", size);
	drm_gem_object_unreference_unlocked(&xen_obj->base);
	return ERR_PTR(ret);
}

static struct xen_gem_object *xendrm_gem_create_with_handle(
	struct drm_file *file_priv, struct drm_device *dev,
	size_t size, uint32_t *handle)
{
	struct xen_gem_object *xen_obj;
	struct drm_gem_object *gem_obj;
	int ret;

	DRM_ERROR("%s size %zu\n", __FUNCTION__, size);
	xen_obj = xendrm_gem_create(dev, size);
	if (IS_ERR(xen_obj))
		return xen_obj;
	gem_obj = &xen_obj->base;
	ret = drm_gem_handle_create(file_priv, gem_obj, handle);
	drm_gem_object_unreference_unlocked(gem_obj);
	if (ret)
		return ERR_PTR(ret);
	return xen_obj;
}

int xendrm_gem_dumb_create(struct drm_file *file_priv,
	struct drm_device *dev, struct drm_mode_create_dumb *args)
{
	struct xen_gem_object *xen_obj;

	args->pitch = DIV_ROUND_UP(args->width * args->bpp, 8);
	args->size = args->pitch * args->height;

	xen_obj = xendrm_gem_create_with_handle(file_priv, dev, args->size,
		&args->handle);
	DRM_ERROR("%s handle %d\n", __FUNCTION__, args->handle);
	return PTR_ERR_OR_ZERO(xen_obj);
}

void xendrm_gem_free_object(struct drm_gem_object *gem_obj)
{
	struct xen_gem_object *xen_obj = to_xen_gem_obj(gem_obj);

	DRM_ERROR("%s gem_obj %p\n", __FUNCTION__, gem_obj);
	xendrm_gem_free(xen_obj);
	drm_gem_object_release(gem_obj);
	kfree(xen_obj);
}

struct sg_table *xendrm_gem_get_sg_table(struct drm_gem_object *gem_obj)
{
	struct xen_gem_object *xen_obj = to_xen_gem_obj(gem_obj);
	struct sg_table *sgt;
	struct scatterlist *src, *dst;
	int ret, i;

	DRM_ERROR("%s gem_obj %p\n", __FUNCTION__, gem_obj);
	DRM_ERROR("%s xen_obj->sgt %p xen_obj->sgt_imported %p xen_obj->sgt_ext %p\n",
			__FUNCTION__, xen_obj->sgt, xen_obj->sgt_imported, xen_obj->sgt_ext);
	if (!xen_obj->sgt)
		return NULL;
	sgt = kzalloc(sizeof(*sgt), GFP_KERNEL);
	if (!sgt)
		return NULL;
	ret = sg_alloc_table(sgt, xen_obj->sgt->nents, GFP_KERNEL);
	if (ret < 0)
		goto fail;
	src = xen_obj->sgt->sgl;
	dst = sgt->sgl;
	for (i = 0; i < xen_obj->sgt->nents; i++) {
		sg_set_page(dst, sg_page(src), src->length, 0);
		dst = sg_next(dst);
		src = sg_next(src);
	}
	return sgt;

fail:
	sg_free_table(sgt);
	kfree(sgt);
	return NULL;
}

struct drm_gem_object *xendrm_gem_import_sg_table(struct drm_device *dev,
	struct dma_buf_attachment *attach, struct sg_table *sgt)
{
	struct xen_gem_object *xen_obj;

	DRM_ERROR("%s attach->dmabuf->size %zu\n", __FUNCTION__, attach->dmabuf->size);
	xen_obj = xendrm_gem_create_obj(dev, attach->dmabuf->size);
	if (IS_ERR(xen_obj))
		return ERR_CAST(xen_obj);
	xen_obj->sgt_imported = sgt;
	DRM_ERROR("%s xen_obj->sgt %p xen_obj->sgt_imported %p xen_obj->sgt_ext %p\n",
			__FUNCTION__, xen_obj->sgt, xen_obj->sgt_imported, xen_obj->sgt_ext);
	return &xen_obj->base;
}

void xendrm_gem_set_ext_sg_table(struct drm_gem_object *gem_obj,
	struct sg_table *sgt)
{
	struct xen_gem_object *xen_obj = to_xen_gem_obj(gem_obj);

	xen_obj->sgt_ext = sgt;
	DRM_ERROR("%s xen_obj->sgt %p xen_obj->sgt_imported %p xen_obj->sgt_ext %p\n",
			__FUNCTION__, xen_obj->sgt, xen_obj->sgt_imported, xen_obj->sgt_ext);
}

static struct xen_fb *xendrm_gem_fb_alloc(struct drm_device *dev,
	const struct drm_mode_fb_cmd2 *mode_cmd,
	struct xen_gem_object *xen_obj,
	const struct drm_framebuffer_funcs *funcs)
{
	struct xen_fb *xen_fb;
	int ret;

	xen_fb = kzalloc(sizeof(*xen_fb), GFP_KERNEL);
	if (!xen_fb)
		return ERR_PTR(-ENOMEM);

	drm_helper_mode_fill_fb_struct(&xen_fb->fb, mode_cmd);
	xen_fb->xen_obj = xen_obj;
	ret = drm_framebuffer_init(dev, &xen_fb->fb, funcs);
	if (ret < 0) {
		DRM_ERROR("Failed to initialize framebuffer: %d\n", ret);
		kfree(xen_fb);
		return ERR_PTR(ret);
	}
	return xen_fb;
}

struct drm_framebuffer *xendrm_gem_fb_create_with_funcs(struct drm_device *dev,
	struct drm_file *file_priv, const struct drm_mode_fb_cmd2 *mode_cmd,
	const struct drm_framebuffer_funcs *funcs)
{
	struct xen_fb *xen_fb;
	struct xen_gem_object *xen_obj;
	struct drm_gem_object *gem_obj;
	unsigned int hsub;
	unsigned int vsub;
	unsigned int min_size;
	int ret;

	/* we do not support formats that require more than 1 plane */
	if (drm_format_num_planes(mode_cmd->pixel_format) != 1) {
		DRM_ERROR("Unsupported pixel format 0x%04x\n",
			mode_cmd->pixel_format);
		return ERR_PTR(-EINVAL);
	}
	hsub = drm_format_horz_chroma_subsampling(mode_cmd->pixel_format);
	vsub = drm_format_vert_chroma_subsampling(mode_cmd->pixel_format);

	gem_obj = drm_gem_object_lookup(file_priv, mode_cmd->handles[0]);
	if (!gem_obj) {
		DRM_ERROR("Failed to lookup GEM object\n");
		return ERR_PTR(-ENXIO);
	}

	min_size = (mode_cmd->height - 1) * mode_cmd->pitches[0] +
		mode_cmd->width *
		drm_format_plane_cpp(mode_cmd->pixel_format, 0) +
		mode_cmd->offsets[0];
	if (gem_obj->size < min_size) {
		drm_gem_object_unreference_unlocked(gem_obj);
		return ERR_PTR(-EINVAL);
	}
	xen_obj = to_xen_gem_obj(gem_obj);

	xen_fb = xendrm_gem_fb_alloc(dev, mode_cmd, xen_obj, funcs);
	if (IS_ERR(xen_fb)) {
		ret = PTR_ERR(xen_fb);
		goto fail;
	}
	return &xen_fb->fb;

fail:
	drm_gem_object_unreference_unlocked(gem_obj);
	return ERR_PTR(ret);
}

void xendrm_gem_fb_destroy(struct drm_framebuffer *fb)
{
	struct xen_fb *xen_fb = to_xen_fb(fb);

	if (xen_fb->xen_obj)
		drm_gem_object_unreference_unlocked(&xen_fb->xen_obj->base);
	drm_framebuffer_cleanup(fb);
	kfree(xen_fb);
}

int xendrm_gem_dumb_map_offset(struct drm_file *file_priv,
	struct drm_device *dev, uint32_t handle, uint64_t *offset)
{
	struct drm_gem_object *gem_obj;
	struct xen_gem_object *xen_obj;


	gem_obj = drm_gem_object_lookup(file_priv, handle);
	DRM_ERROR("%s gem_obj %p handle %d\n", __FUNCTION__, gem_obj, handle);
	if (!gem_obj) {
		DRM_ERROR("Failed to lookup GEM object\n");
		return -EINVAL;
	}
	xen_obj = to_xen_gem_obj(gem_obj);
	/* do not allow mapping of the imported buffers */
	if (xen_obj->base.import_attach) {
		drm_gem_object_unreference_unlocked(gem_obj);
		return -EINVAL;
	}
	*offset = drm_vma_node_offset_addr(&gem_obj->vma_node);
	drm_gem_object_unreference_unlocked(gem_obj);
	return 0;
}

static int xendrm_mmap_sgt(struct device *dev, struct sg_table *sgt,
	struct vm_area_struct *vma)
{
	unsigned long addr = vma->vm_start;
	unsigned long offset = vma->vm_pgoff << PAGE_SHIFT;
	struct scatterlist *sg;
	int ret, i;

	DRM_ERROR("%s\n", __FUNCTION__);
	for_each_sg(sgt->sgl, sg, sgt->nents, i) {
		struct page *page = sg_page(sg);
		unsigned long remainder = vma->vm_end - addr;
		unsigned long len = sg->length;

		if (offset >= sg->length) {
			offset -= sg->length;
			continue;
		} else if (offset) {
			page += offset / PAGE_SIZE;
			len = sg->length - offset;
			offset = 0;
		}
		len = min(len, remainder);
		ret = remap_pfn_range(vma, addr, page_to_pfn(page), len,
			vma->vm_page_prot);
		if (ret < 0)
			return ret;
		addr += len;
		if (addr >= vma->vm_end)
			return 0;
	}
	{
		unsigned long addr = vma->vm_start;
		struct sg_page_iter sg_iter;

		for_each_sg_page(sgt->sgl, &sg_iter, sgt->nents, 0) {
			struct page *page = sg_page_iter_page(&sg_iter);

			ret = vm_insert_page(vma, addr, page);
			addr += PAGE_SIZE;
		}

	}
	return 0;
}

static int xendrm_gem_mmap_obj(struct xen_gem_object *xen_obj,
	struct vm_area_struct *vma)
{
	struct sg_table *sgt;
	int ret;

	DRM_ERROR("%s xen_obj->sgt %p xen_obj->sgt_imported %p xen_obj->sgt_ext %p\n",
			__FUNCTION__, xen_obj->sgt, xen_obj->sgt_imported, xen_obj->sgt_ext);
	/*
	 * Clear the VM_PFNMAP flag that was set by drm_gem_mmap(), and set the
	 * vm_pgoff (used as a fake buffer offset by DRM) to 0 as we want to map
	 * the whole buffer.
	 */
	vma->vm_flags &= ~VM_PFNMAP;
	vma->vm_pgoff = 0;
	/* this is the only way to mmap for unprivileged domain */
	vma->vm_page_prot = PAGE_SHARED;

	if (xen_obj->sgt)
		sgt = xen_obj->sgt;
	else if (xen_obj->sgt_imported)
		sgt = xen_obj->sgt_imported;
	else
		sgt = xen_obj->sgt_ext;
	ret = xendrm_mmap_sgt(xen_obj->base.dev->dev, sgt, vma);
	if (ret < 0) {
		DRM_ERROR("Failed to remap: %d\n", ret);
		drm_gem_vm_close(vma);
	}
	return ret;
}

int xendrm_gem_mmap(struct file *filp, struct vm_area_struct *vma)
{
	struct xen_gem_object *xen_obj;
	struct drm_gem_object *gem_obj;
	int ret;

	ret = drm_gem_mmap(filp, vma);
	if (ret)
		return ret;
	gem_obj = vma->vm_private_data;
	xen_obj = to_xen_gem_obj(gem_obj);
	return xendrm_gem_mmap_obj(xen_obj, vma);
}

void *xendrm_gem_prime_vmap(struct drm_gem_object *gem_obj)
{
	struct xen_gem_object *xen_obj = to_xen_gem_obj(gem_obj);
	struct page **pages;
	size_t num_pages;
	void *vaddr;
	int ret;

	DRM_ERROR("%s xen_obj->sgt %p xen_obj->sgt_imported %p xen_obj->sgt_ext %p\n",
			__FUNCTION__, xen_obj->sgt, xen_obj->sgt_imported, xen_obj->sgt_ext);
	num_pages = DIV_ROUND_UP(xen_obj->size, PAGE_SIZE);
	pages = drm_malloc_ab(num_pages, sizeof(*pages));
	if (!pages)
		return NULL;
	vaddr = NULL;
	ret = drm_prime_sg_to_page_addr_arrays(xen_obj->sgt, pages, NULL,
		num_pages);
	if (ret < 0)
		goto fail;

	vaddr = vmap(pages, num_pages, GFP_KERNEL, PAGE_SHARED);
fail:
	drm_free_large(pages);
	return vaddr;
}

void xendrm_gem_prime_vunmap(struct drm_gem_object *gem_obj, void *vaddr)
{
	vunmap(vaddr);
}

int xendrm_gem_prime_mmap(struct drm_gem_object *gem_obj,
	struct vm_area_struct *vma)
{
	struct xen_gem_object *xen_obj;
	int ret;

	ret = drm_gem_mmap_obj(gem_obj, gem_obj->size, vma);
	if (ret < 0)
		return ret;
	xen_obj = to_xen_gem_obj(gem_obj);
	DRM_ERROR("%s xen_obj->sgt %p xen_obj->sgt_imported %p xen_obj->sgt_ext %p\n",
			__FUNCTION__, xen_obj->sgt, xen_obj->sgt_imported, xen_obj->sgt_ext);
	return xendrm_gem_mmap_obj(xen_obj, vma);
}
