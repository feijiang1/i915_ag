// SPDX-License-Identifier: MIT
/*
 * Copyright © 2019 Intel Corporation
 */

#include <linux/device.h>
#include <linux/sched/mm.h>

#include <drm/drm_drv.h>
#include <linux/sched/mm.h>

#include "i915_drv.h"
#include "i915_utils.h"

#define FDO_BUG_MSG "Please contact your Intel representative."

void
__i915_printk(struct drm_i915_private *dev_priv, const char *level,
	      const char *fmt, ...)
{
	static bool shown_bug_once;
	struct device *kdev = dev_priv->drm.dev;
	bool is_error = level[1] <= KERN_ERR[1];
	bool is_debug = level[1] == KERN_DEBUG[1];
	struct va_format vaf;
	va_list args;

	if (is_debug && !drm_debug_enabled(DRM_UT_DRIVER))
		return;

	va_start(args, fmt);

	vaf.fmt = fmt;
	vaf.va = &args;

	if (is_error)
		dev_printk(level, kdev, "%pV", &vaf);
	else
		dev_printk(level, kdev, "[" DRM_NAME ":%ps] %pV",
			   __builtin_return_address(0), &vaf);

	va_end(args);

	if (is_error && !shown_bug_once) {
		/*
		 * Ask the user to file a bug report for the error, except
		 * if they may have caused the bug by fiddling with unsafe
		 * module parameters.
		 */
		if (!test_taint(TAINT_USER))
			dev_notice(kdev, "%s", FDO_BUG_MSG);
		shown_bug_once = true;
	}
}

void add_taint_for_CI(struct drm_i915_private *i915, unsigned int taint)
{
	__i915_printk(i915, KERN_NOTICE, "CI tainted:%#x by %pS\n",
		      taint, (void *)_RET_IP_);

	/* Failures that occur during fault injection testing are expected */
	if (!i915_error_injected())
		__add_taint_for_CI(taint);
}

#if IS_ENABLED(CPTCFG_DRM_I915_DEBUG)
static int i915_probe_fail_count;

int __i915_inject_probe_error(struct drm_i915_private *i915, int err,
			      const char *func, int line)
{
	if (i915_probe_fail_count >= i915_modparams.inject_probe_failure)
		return 0;

	if (++i915_probe_fail_count < i915_modparams.inject_probe_failure)
		return 0;

	__i915_printk(i915, KERN_INFO,
		      "Injecting failure %d at checkpoint %u [%s:%d]\n",
		      err, i915_modparams.inject_probe_failure, func, line);
	i915_modparams.inject_probe_failure = 0;
	return err;
}

bool i915_error_injected(void)
{
	return i915_probe_fail_count > i915_modparams.inject_probe_failure;
}

#endif

void cancel_timer(struct timer_list *t)
{
	if (!timer_active(t))
		return;

	del_timer(t);
	WRITE_ONCE(t->expires, 0);
}

void set_timer_ms(struct timer_list *t, unsigned long timeout)
{
	if (!timeout) {
		cancel_timer(t);
		return;
	}

	timeout = msecs_to_jiffies(timeout);

	/*
	 * Paranoia to make sure the compiler computes the timeout before
	 * loading 'jiffies' as jiffies is volatile and may be updated in
	 * the background by a timer tick. All to reduce the complexity
	 * of the addition and reduce the risk of losing a jiffie.
	 */
	barrier();

	/* Keep t->expires = 0 reserved to indicate a canceled timer. */
	mod_timer(t, jiffies + timeout ?: 1);
}

bool i915_vtd_active(struct drm_i915_private *i915)
{
	if (device_iommu_mapped(i915->drm.dev))
		return true;

	/* Running as a guest, we assume the host is enforcing VT'd */
	return i915_run_as_guest();
}

void fs_reclaim_taints_mutex(struct mutex *mutex)
{
	if (!IS_ENABLED(CONFIG_LOCKDEP))
		return;

	fs_reclaim_acquire(GFP_KERNEL);

	mutex_acquire(&mutex->dep_map, 0, 0, _RET_IP_);
	mutex_release(&mutex->dep_map, _RET_IP_);

	fs_reclaim_release(GFP_KERNEL);
}

#ifdef CONFIG_DEBUG_LOCK_ALLOC
void __mark_lock_used_irq(struct lockdep_map *lock)
{
	/*
	 * Due to an interesting quirk in lockdep's internal debug tracking,
	 * after setting a subclass we must ensure the lock is used. Otherwise,
	 * nr_unused_locks is incremented once too often.
	 */
	local_irq_disable();
	lock_map_acquire(lock);
	lock_map_release(lock);
	local_irq_enable();
}
#endif

/**
 * from_user_to_u32array - convert user input into array of u32
 * @from: user input
 * @count: number of characters to read
 * @array: array with results
 * @size: size of the array
 *
 * We expect input formatted as comma-separated list of integer values.
 *
 * Returns number of entries parsed or negative errno on failure.
 */
int from_user_to_u32array(const char __user *from, size_t count,
			  u32 *array, unsigned int size)
{
	unsigned int num = 0;
	char *buf, *p, save;
	int ret;

	/* [(sign + longest representation) + comma] + newline + terminator */
	if (count > (1 + sizeof(u32) * 8 + 1) * size + 1 + 1)
		return -EFBIG;

	p = buf = kzalloc(count + 1, GFP_USER);
	if (!buf)
		return -ENOMEM;

	if (copy_from_user(buf, from, count)) {
		ret = -EFAULT;
		goto out_free;
	}

	do {
		int len;

		if (num == size) {
			ret = -EINVAL;
			goto out_free;
		}
		len = strcspn(p, ",");

		/* nul-terminate and parse */
		save = p[len];
		p[len] = '\0';

		ret = kstrtou32(p, 0, &array[num]);
		if (ret)
			goto out_free;

		p += len + 1;
		num++;
	} while (save == ',');

	ret = num;
out_free:
	kfree(buf);
	return ret;
}
