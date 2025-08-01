// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) 2020 Intel
 *
 * Based on drivers/base/devres.c
 */

#include <drm/drm_managed.h>

#include <linux/export.h>
#include <linux/list.h>
#include <linux/mutex.h>
#include <linux/slab.h>
#include <linux/spinlock.h>

#include <drm/drm_device.h>
#include <drm/drm_print.h>

#include "drm_internal.h"

/**
 * DOC: managed resources
 *
 * Inspired by struct &device managed resources, but tied to the lifetime of
 * struct &drm_device, which can outlive the underlying physical device, usually
 * when userspace has some open files and other handles to resources still open.
 *
 * Release actions can be added with drmm_add_action(), memory allocations can
 * be done directly with drmm_kmalloc() and the related functions. Everything
 * will be released on the final drm_dev_put() in reverse order of how the
 * release actions have been added and memory has been allocated since driver
 * loading started with devm_drm_dev_alloc().
 *
 * Note that release actions and managed memory can also be added and removed
 * during the lifetime of the driver, all the functions are fully concurrent
 * safe. But it is recommended to use managed resources only for resources that
 * change rarely, if ever, during the lifetime of the &drm_device instance.
 */

struct drmres_node {
	struct list_head	entry;
	drmres_release_t	release;
	const char		*name;
	size_t			size;
};

struct drmres {
	struct drmres_node		node;
	/*
	 * Some archs want to perform DMA into kmalloc caches
	 * and need a guaranteed alignment larger than
	 * the alignment of a 64-bit integer.
	 * Thus we use ARCH_DMA_MINALIGN for data[] which will force the same
	 * alignment for struct drmres when allocated by kmalloc().
	 */
	u8 __aligned(ARCH_DMA_MINALIGN) data[];
};

static void free_dr(struct drmres *dr)
{
	kfree_const(dr->node.name);
	kfree(dr);
}

void drm_managed_release(struct drm_device *dev)
{
	struct drmres *dr, *tmp;

	drm_dbg_drmres(dev, "drmres release begin\n");
	list_for_each_entry_safe(dr, tmp, &dev->managed.resources, node.entry) {
		drm_dbg_drmres(dev, "REL %p %s (%zu bytes)\n",
			       dr, dr->node.name, dr->node.size);

		if (dr->node.release)
			dr->node.release(dev, dr->node.size ? *(void **)&dr->data : NULL);

		list_del(&dr->node.entry);
		free_dr(dr);
	}
	drm_dbg_drmres(dev, "drmres release end\n");
}

/*
 * Always inline so that kmalloc_track_caller tracks the actual interesting
 * caller outside of drm_managed.c.
 */
static __always_inline struct drmres * alloc_dr(drmres_release_t release,
						size_t size, gfp_t gfp, int nid)
{
	size_t tot_size;
	struct drmres *dr;

	/* We must catch any near-SIZE_MAX cases that could overflow. */
	if (unlikely(check_add_overflow(sizeof(*dr), size, &tot_size)))
		return NULL;

	dr = kmalloc_node_track_caller(tot_size, gfp, nid);
	if (unlikely(!dr))
		return NULL;

	memset(dr, 0, offsetof(struct drmres, data));

	INIT_LIST_HEAD(&dr->node.entry);
	dr->node.release = release;
	dr->node.size = size;

	return dr;
}

static void del_dr(struct drm_device *dev, struct drmres *dr)
{
	list_del_init(&dr->node.entry);

	drm_dbg_drmres(dev, "DEL %p %s (%lu bytes)\n",
		       dr, dr->node.name, (unsigned long) dr->node.size);
}

static void add_dr(struct drm_device *dev, struct drmres *dr)
{
	unsigned long flags;

	spin_lock_irqsave(&dev->managed.lock, flags);
	list_add(&dr->node.entry, &dev->managed.resources);
	spin_unlock_irqrestore(&dev->managed.lock, flags);

	drm_dbg_drmres(dev, "ADD %p %s (%lu bytes)\n",
		       dr, dr->node.name, (unsigned long) dr->node.size);
}

void drmm_add_final_kfree(struct drm_device *dev, void *container)
{
	WARN_ON(dev->managed.final_kfree);
	WARN_ON(dev < (struct drm_device *) container);
	WARN_ON(dev + 1 > (struct drm_device *) (container + ksize(container)));
	dev->managed.final_kfree = container;
}

int __drmm_add_action(struct drm_device *dev,
		      drmres_release_t action,
		      void *data, const char *name)
{
	struct drmres *dr;
	void **void_ptr;

	dr = alloc_dr(action, data ? sizeof(void*) : 0,
		      GFP_KERNEL | __GFP_ZERO,
		      dev_to_node(dev->dev));
	if (!dr) {
		drm_dbg_drmres(dev, "failed to add action %s for %p\n",
			       name, data);
		return -ENOMEM;
	}

	dr->node.name = kstrdup_const(name, GFP_KERNEL);
	if (data) {
		void_ptr = (void **)&dr->data;
		*void_ptr = data;
	}

	add_dr(dev, dr);

	return 0;
}
EXPORT_SYMBOL(__drmm_add_action);

int __drmm_add_action_or_reset(struct drm_device *dev,
			       drmres_release_t action,
			       void *data, const char *name)
{
	int ret;

	ret = __drmm_add_action(dev, action, data, name);
	if (ret)
		action(dev, data);

	return ret;
}
EXPORT_SYMBOL(__drmm_add_action_or_reset);

/**
 * drmm_release_action - release a managed action from a &drm_device
 * @dev: DRM device
 * @action: function which would be called when @dev is released
 * @data: opaque pointer, passed to @action
 *
 * This function calls the @action previously added by drmm_add_action()
 * immediately.
 * The @action is removed from the list of cleanup actions for @dev,
 * which means that it won't be called in the final drm_dev_put().
 */
void drmm_release_action(struct drm_device *dev,
			 drmres_release_t action,
			 void *data)
{
	struct drmres *dr_match = NULL, *dr;
	unsigned long flags;

	spin_lock_irqsave(&dev->managed.lock, flags);
	list_for_each_entry_reverse(dr, &dev->managed.resources, node.entry) {
		if (dr->node.release == action) {
			if (!data || *(void **)dr->data == data) {
				dr_match = dr;
				del_dr(dev, dr_match);
				break;
			}
		}
	}
	spin_unlock_irqrestore(&dev->managed.lock, flags);

	if (WARN_ON(!dr_match))
		return;

	action(dev, data);

	free_dr(dr_match);
}
EXPORT_SYMBOL(drmm_release_action);

/**
 * drmm_kmalloc - &drm_device managed kmalloc()
 * @dev: DRM device
 * @size: size of the memory allocation
 * @gfp: GFP allocation flags
 *
 * This is a &drm_device managed version of kmalloc(). The allocated memory is
 * automatically freed on the final drm_dev_put(). Memory can also be freed
 * before the final drm_dev_put() by calling drmm_kfree().
 */
void *drmm_kmalloc(struct drm_device *dev, size_t size, gfp_t gfp)
{
	struct drmres *dr;

	dr = alloc_dr(NULL, size, gfp, dev_to_node(dev->dev));
	if (!dr) {
		drm_dbg_drmres(dev, "failed to allocate %zu bytes, %u flags\n",
			       size, gfp);
		return NULL;
	}
	dr->node.name = kstrdup_const("kmalloc", gfp);

	add_dr(dev, dr);

	return dr->data;
}
EXPORT_SYMBOL(drmm_kmalloc);

/**
 * drmm_kstrdup - &drm_device managed kstrdup()
 * @dev: DRM device
 * @s: 0-terminated string to be duplicated
 * @gfp: GFP allocation flags
 *
 * This is a &drm_device managed version of kstrdup(). The allocated memory is
 * automatically freed on the final drm_dev_put() and works exactly like a
 * memory allocation obtained by drmm_kmalloc().
 */
char *drmm_kstrdup(struct drm_device *dev, const char *s, gfp_t gfp)
{
	size_t size;
	char *buf;

	if (!s)
		return NULL;

	size = strlen(s) + 1;
	buf = drmm_kmalloc(dev, size, gfp);
	if (buf)
		memcpy(buf, s, size);
	return buf;
}
EXPORT_SYMBOL_GPL(drmm_kstrdup);

/**
 * drmm_kfree - &drm_device managed kfree()
 * @dev: DRM device
 * @data: memory allocation to be freed
 *
 * This is a &drm_device managed version of kfree() which can be used to
 * release memory allocated through drmm_kmalloc() or any of its related
 * functions before the final drm_dev_put() of @dev.
 */
void drmm_kfree(struct drm_device *dev, void *data)
{
	struct drmres *dr_match = NULL, *dr;
	unsigned long flags;

	if (!data)
		return;

	spin_lock_irqsave(&dev->managed.lock, flags);
	list_for_each_entry(dr, &dev->managed.resources, node.entry) {
		if (dr->data == data) {
			dr_match = dr;
			del_dr(dev, dr_match);
			break;
		}
	}
	spin_unlock_irqrestore(&dev->managed.lock, flags);

	if (WARN_ON(!dr_match))
		return;

	free_dr(dr_match);
}
EXPORT_SYMBOL(drmm_kfree);

void __drmm_mutex_release(struct drm_device *dev, void *res)
{
	struct mutex *lock = res;

	mutex_destroy(lock);
}
EXPORT_SYMBOL(__drmm_mutex_release);

void __drmm_workqueue_release(struct drm_device *device, void *res)
{
	struct workqueue_struct *wq = res;

	destroy_workqueue(wq);
}
EXPORT_SYMBOL(__drmm_workqueue_release);
