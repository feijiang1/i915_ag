/*
 * SPDX-License-Identifier: MIT
 *
 * Copyright © 2016 Intel Corporation
 */

#include <linux/dma-fence-array.h>
#include <linux/dma-fence-chain.h>
#include <linux/jiffies.h>

#include "gt/intel_engine.h"

#include "dma_resv_utils.h"
#include "i915_suspend_fence.h"
#include "i915_gem_ioctls.h"
#include "i915_gem_object.h"

static long
i915_gem_object_wait_fence(struct dma_fence *fence,
			   unsigned int flags,
			   long timeout)
{
	BUILD_BUG_ON(I915_WAIT_INTERRUPTIBLE != 0x1);

	if (dma_fence_is_lr(fence) ||
	    dma_fence_is_suspend(fence))
		return -EINVAL;

	if (test_bit(DMA_FENCE_FLAG_SIGNALED_BIT, &fence->flags))
		return timeout;

	if (dma_fence_is_i915(fence))
		return i915_request_wait(to_request(fence), flags, timeout);

	return dma_fence_wait_timeout(fence,
				      flags & I915_WAIT_INTERRUPTIBLE,
				      timeout);
}

static long
i915_gem_object_wait_reservation(struct dma_resv *resv,
				 unsigned int flags,
				 long timeout)
{
#ifdef BPM_DMA_RESV_ITER_BEGIN_PRESENT
	struct dma_resv_iter cursor;
	struct dma_fence *fence;
	long ret = timeout ?: 1;

	dma_resv_iter_begin(&cursor, resv,
			    dma_resv_usage_rw(flags & I915_WAIT_ALL));
	dma_resv_for_each_fence_unlocked(&cursor, fence) {
		ret = i915_gem_object_wait_fence(fence, flags, timeout);
		if (ret <= 0)
			break;

		if (timeout)
			timeout = ret;

	}
	dma_resv_iter_end(&cursor);
	return ret;
#else
	struct dma_fence *excl;
	bool prune_fences = false;

	if (flags & I915_WAIT_ALL) {
		struct dma_fence **shared;
		unsigned int count, i;
		int ret;

		ret = dma_resv_get_fences(resv, &excl, &count, &shared);
		if (ret)
			return ret;

		for (i = 0; i < count; i++) {
			timeout = i915_gem_object_wait_fence(shared[i],
							     flags, timeout);
			if (timeout < 0)
				break;

			dma_fence_put(shared[i]);
		}

		for (; i < count; i++)
			dma_fence_put(shared[i]);
		kfree(shared);
		
		/*
		 * If both shared fences and an exclusive fence exist,
		 * then by construction the shared fences must be later
		 * than the exclusive fence. If we successfully wait for
		 * all the shared fences, we know that the exclusive fence
		 * must all be signaled. If all the shared fences are
		 * signaled, we can prune the array and recover the
		 * floating references on the fences/requests.
		 */
		prune_fences = count && timeout >= 0;
	} else {
		excl = dma_resv_get_excl_unlocked(resv);
	}

	if (excl && timeout >= 0)
		timeout = i915_gem_object_wait_fence(excl, flags, timeout);

	dma_fence_put(excl);

	/*
	 * Opportunistically prune the fences iff we know they have *all* been
	 * signaled.
	 */
	if (prune_fences)
		dma_resv_prune(resv);

	return timeout;
#endif
}

static void fence_set_priority(struct dma_fence *fence, int prio)
{
	if (dma_fence_is_signaled(fence) || !dma_fence_is_i915(fence))
		return;

	i915_request_set_priority(to_request(fence), prio);
}

static inline bool __dma_fence_is_chain(const struct dma_fence *fence)
{
	return fence->ops == &dma_fence_chain_ops;
}

void i915_gem_fence_wait_priority(struct dma_fence *fence, int prio)
{
	if (dma_fence_is_signaled(fence))
		return;

	local_bh_disable();

	/* Recurse once into a fence-array */
	if (dma_fence_is_array(fence)) {
		struct dma_fence_array *array = to_dma_fence_array(fence);
		int i;

		for (i = 0; i < array->num_fences; i++)
			fence_set_priority(array->fences[i], prio);
	} else if (__dma_fence_is_chain(fence)) {
		struct dma_fence *iter;

		/* The chain is ordered; if we boost the last, we boost all */
		dma_fence_chain_for_each(iter, fence) {
			fence_set_priority(to_dma_fence_chain(iter)->fence,
					   prio);
			break;
		}
		dma_fence_put(iter);
	} else {
		fence_set_priority(fence, prio);
	}

	local_bh_enable(); /* kick the tasklets if queues were reprioritised */
}

int
i915_gem_object_wait_priority(struct drm_i915_gem_object *obj,
			      unsigned int flags,
			      int prio)
{
#ifdef BPM_DMA_RESV_ITER_BEGIN_PRESENT
	struct dma_resv_iter cursor;
	struct dma_fence *fence;

	dma_resv_iter_begin(&cursor, obj->base.resv, flags & I915_WAIT_ALL);
	dma_resv_for_each_fence_unlocked(&cursor, fence)
		i915_gem_fence_wait_priority(fence, prio);
	dma_resv_iter_end(&cursor);
#else
	struct dma_fence *excl;

	if (flags & I915_WAIT_ALL) {
		struct dma_fence **shared;
		unsigned int count, i;
		int ret;

		ret = dma_resv_get_fences(obj->base.resv, &excl, &count,
					  &shared);
		if (ret)
			return ret;

		for (i = 0; i < count; i++) {
			i915_gem_fence_wait_priority(shared[i], prio);
			dma_fence_put(shared[i]);
		}

		kfree(shared);
	} else {
		excl = dma_resv_get_excl_unlocked(obj->base.resv);
	}

	if (excl) {
		i915_gem_fence_wait_priority(excl, prio);
		dma_fence_put(excl);
	}
#endif
	return 0;
}

/**
 * Waits for rendering to the object to be completed
 * @obj: i915 gem object
 * @flags: how to wait (under a lock, for all rendering or just for writes etc)
 * @timeout: how long to wait
 */
long
__i915_gem_object_wait(struct drm_i915_gem_object *obj,
		       unsigned int flags,
		       long timeout)
{
	might_sleep_if(timeout > 0);
	if (GEM_WARN_ON(timeout < 0))
		return timeout;

	timeout = i915_gem_object_migrate_wait(obj, flags, timeout);
	if (timeout < 0)
		return timeout;

	return i915_gem_object_wait_reservation(obj->base.resv, flags, timeout);
}

int
i915_gem_object_wait(struct drm_i915_gem_object *obj,
		     unsigned int flags,
		     long timeout)
{
	timeout = __i915_gem_object_wait(obj, flags, timeout);
	return timeout < 0 ? timeout : 0;
}

bool i915_gem_object_is_active(struct drm_i915_gem_object *obj)
{
	if (i915_gem_object_has_migrate(obj))
		return true;

	return !dma_resv_test_signaled(obj->base.resv, true);
}

static inline unsigned long nsecs_to_jiffies_timeout(const u64 n)
{
	/* nsecs_to_jiffies64() does not guard against overflow */
	if (NSEC_PER_SEC % HZ &&
	    div_u64(n, NSEC_PER_SEC) >= MAX_JIFFY_OFFSET / HZ)
		return MAX_JIFFY_OFFSET;

	return min_t(u64, MAX_JIFFY_OFFSET, nsecs_to_jiffies64(n) + 1);
}

static unsigned long to_wait_timeout(s64 timeout_ns)
{
	if (timeout_ns < 0)
		return MAX_SCHEDULE_TIMEOUT;

	if (timeout_ns == 0)
		return 0;

	return nsecs_to_jiffies_timeout(timeout_ns);
}

/**
 * i915_gem_wait_ioctl - implements DRM_IOCTL_I915_GEM_WAIT
 * @dev: drm device pointer
 * @data: ioctl data blob
 * @file: drm file pointer
 *
 * Returns 0 if successful, else an error is returned with the remaining time in
 * the timeout parameter.
 *  -ETIME: object is still busy after timeout
 *  -ERESTARTSYS: signal interrupted the wait
 *  -ENONENT: object doesn't exist
 * Also possible, but rare:
 *  -EAGAIN: incomplete, restart syscall
 *  -ENOMEM: damn
 *  -ENODEV: Internal IRQ fail
 *  -E?: The add request failed
 *
 * The wait ioctl with a timeout of 0 reimplements the busy ioctl. With any
 * non-zero timeout parameter the wait ioctl will wait for the given number of
 * nanoseconds on an object becoming unbusy. Since the wait itself does so
 * without holding struct_mutex the object may become re-busied before this
 * function completes. A similar but shorter * race condition exists in the busy
 * ioctl
 */
int
i915_gem_wait_ioctl(struct drm_device *dev, void *data, struct drm_file *file)
{
	struct drm_i915_gem_wait *args = data;
	struct drm_i915_gem_object *obj;
	ktime_t start;
	long ret;

	if (args->flags != 0)
		return -EINVAL;

	obj = i915_gem_object_lookup(file, args->bo_handle);
	if (!obj)
		return -ENOENT;

	start = ktime_get();

	ret = i915_gem_object_wait(obj,
				   I915_WAIT_INTERRUPTIBLE |
				   I915_WAIT_PRIORITY |
				   I915_WAIT_ALL,
				   to_wait_timeout(args->timeout_ns));

	if (args->timeout_ns > 0) {
		args->timeout_ns -= ktime_to_ns(ktime_sub(ktime_get(), start));
		if (args->timeout_ns < 0)
			args->timeout_ns = 0;

		/*
		 * Apparently ktime isn't accurate enough and occasionally has a
		 * bit of mismatch in the jiffies<->nsecs<->ktime loop. So patch
		 * things up to make the test happy. We allow up to 1 jiffy.
		 *
		 * This is a regression from the timespec->ktime conversion.
		 */
		if (ret == -ETIME && !nsecs_to_jiffies(args->timeout_ns))
			args->timeout_ns = 0;

		/* Asked to wait beyond the jiffie/scheduler precision? */
		if (ret == -ETIME && args->timeout_ns)
			ret = -EAGAIN;
	}

	i915_gem_object_put(obj);
	return ret;
}
