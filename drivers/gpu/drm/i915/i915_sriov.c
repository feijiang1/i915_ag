// SPDX-License-Identifier: MIT
/*
 * Copyright © 2022 Intel Corporation
 */

#ifdef BPM_VFIO_SR_IOV_VF_MIGRATION_NOT_PRESENT
#include "i915_sriov.h"
#else
#include <drm/i915_sriov.h>
#endif

#include "gt/intel_tlb.h"
#include "i915_sriov_sysfs.h"
#include "i915_debugger.h"
#include "i915_drv.h"
#include "i915_irq.h"
#include "i915_pci.h"
#include "i915_utils.h"
#include "intel_pci_config.h"
#include "gem/i915_gem_pm.h"
#include "gem/i915_gem_context.h"

#include "gt/intel_context.h"
#include "gt/intel_engine_heartbeat.h"
#include "gt/intel_gt.h"
#include "gt/intel_gt_pm.h"
#include "gt/iov/intel_iov_migration.h"
#include "gt/iov/intel_iov_provisioning.h"
#include "gt/iov/intel_iov_service.h"
#include "gt/iov/intel_iov_reg.h"
#include "gt/iov/intel_iov_state.h"
#include "gt/iov/intel_iov_utils.h"

/* XXX: We need to dig into PCI-core internals to be able to implement VF BAR resize */
#include "../../../pci/pci.h"

/**
 * DOC: VM Migration with SR-IOV
 *
 * Most VMM applications allow to store state of a VM, and restore it
 * at different time or on another machine. To allow proper migration of a
 * VM which configuration includes directly attached VF device, we need to
 * assure that VF state is part of the VM image being migrated.
 *
 * Storing complete state of any hardware is extremely hard. Since the migrated
 * VF state might be incomplete, we need to do proper re-initialization of VF
 * device on the target machine. This initialization is done within
 * `VF Post-migration worker`_.
 */

/**
 * DOC: VF Post-migration worker
 *
 * After `VM Migration with SR-IOV`_, i915 ends up running on a new VF device
 * which was just reset using FLR. While the platform model and memory sizes
 * assigned to this new VF must match the previous, address of Global GTT chunk
 * assigned to the new VF might be different. At that point, contexts and
 * doorbells are no longer registered to GuC and thus their state is invalid.
 * Communication with the GuC is also no longer fully operational.
 *
 * The new GuC informs the VF driver that migration just happened, by setting
 * `GUC_CTB_STATUS_MIGRATED`_ bit in `CTB Descriptor`_, and responding with
 * `INTEL_GUC_RESPONSE_VF_MIGRATED`_ error to at least one request. When VF driver
 * notices any of these, it schedules post-migration worker. The worker makes
 * sure it is executed at most once per migration, by bailing out in case it
 * was scheduled again while re-establishing GuC communications.
 *
 * The post-migration worker has two main goals:
 *
 * * Update driver state to prepare work on a new hardware (treated as new
 *   even if the VM got restored at the place where it worked before).
 *
 * * Provide users with experience which is as close as possible to being
 *   seamless (in terms of failed kernel calls and corrupted buffers).
 *
 * To achieve these goals, the following operations need to be performed:
 *
 * * Get new provisioning information from GuC. While count of the provisioned
 *   resources must match the previous VM instance, the start point might be
 *   different, and for non-virtualized ones that is significant.
 *
 * * Apply fixups to prepare work on new ranges of non-virtualized resources.
 *   This really only concerns Global GTT, as it only has one address space
 *   shared between PF and all VFs.
 *
 * * Clear state information which depended on the previous hardware and is no
 *   longer valid. This concerns state of requests which were in-flight while the
 *   migration happened, but also registration to GuC of both the requests and
 *   contexts. These must be marked as non-submitted and non-registered, and then
 *   re-registered to the new GuC.
 *
 * * Prevent any kernel workers from trying to perform the standard VF driver
 *   operations before the fixups are fully applied. These workers operate as
 *   separate threads, so they could try to access various driver structures
 *   before they are ready.
 *
 * * Provide seamless switch for the user space, by blocking any IOCTLs during
 *   migration and getting back to them when the fixups are applied and the VF
 *   driver is ready.
 *
 * The post-migration worker performs the operations above in proper order to
 * ensure safe transition. First it does a shutdown of any other driver operations
 * and hardware-related states. Then it does handshake for
 * `GuC MMIO based communication`_, and receives new provisioning data through
 * that channel. With the new GGTT range taken from provisioning, the worker
 * rebases 'Virtual Memory Address'_ structures used for tracking GGTT allocations,
 * by shifting addresses of the underlying `drm_mm`_ nodes to range newly
 * assigned to this VF. After the fixups are applied, the VF driver is
 * kickstarted back into ready state. Contexts are re-registered to GuC, then
 * User space calls as well as internal operations are resumed. If there are
 * any requests which were moved back to scheduled list, they are re-submitted
 * by the tasklet soon after post-migration worker ends.
 */

/* safe for use before register access via uncore is completed */
static u32 pci_peek_mmio_read32(struct pci_dev *pdev, i915_reg_t reg)
{
	unsigned long offset = i915_mmio_reg_offset(reg);
	void __iomem *addr;
	u32 value;

	addr = pci_iomap_range(pdev, 0, offset, sizeof(u32));
	if (WARN(!addr, "Failed to map MMIO at %#lx\n", offset))
		return 0;

	value = readl(addr);
	pci_iounmap(pdev, addr);

	return value;
}

static bool gen12_pci_capability_is_vf(struct pci_dev *pdev)
{
	u32 value = pci_peek_mmio_read32(pdev, GEN12_VF_CAP_REG);

	/*
	 * Bugs in PCI programming (or failing hardware) can occasionally cause
	 * lost access to the MMIO BAR.  When this happens, register reads will
	 * come back with 0xFFFFFFFF for every register, including VF_CAP, and
	 * then we may wrongly claim that we are running on the VF device.
	 * Since VF_CAP has only one bit valid, make sure no other bits are set.
	 */
	if (WARN(value & ~GEN12_VF, "MMIO BAR malfunction, %#x returned %#x\n",
		 i915_mmio_reg_offset(GEN12_VF_CAP_REG), value))
		return false;

	return value & GEN12_VF;
}

#ifdef CONFIG_PCI_IOV

static bool works_with_iaf(struct drm_i915_private *i915)
{
	if (!HAS_IAF(i915) || !i915->params.enable_iaf)
		return true;

	/* can't use IS_PLATFORM as RUNTIME_INFO is not ready yet */
	if (INTEL_INFO(i915)->platform == INTEL_PONTEVECCHIO)
		return false;

	return true;
}

static bool wants_pf(struct drm_i915_private *i915)
{
#define ENABLE_GUC_SRIOV_PF		BIT(2)

	if (i915->params.enable_guc < 0)
		return false;

	if (i915->params.enable_guc & ENABLE_GUC_SRIOV_PF) {
		drm_info(&i915->drm,
			 "Don't enable PF with 'enable_guc=%d' - try 'max_vfs=%u' instead\n",
			 i915->params.enable_guc,
			 pci_sriov_get_totalvfs(to_pci_dev(i915->drm.dev)));
		return true;
	}

	return false;
}

static unsigned int wanted_max_vfs(struct drm_i915_private *i915)
{
	/* XXX allow to override "max_vfs" with deprecated "enable_guc" */
	if (wants_pf(i915))
		return ~0;

	return i915->params.max_vfs;
}

static int pf_reduce_totalvfs(struct drm_i915_private *i915, int limit)
{
	int err;

	err = pci_sriov_set_totalvfs(to_pci_dev(i915->drm.dev), limit);
	drm_WARN(&i915->drm, err, "Failed to set number of VFs to %d (%pe)\n",
		 limit, ERR_PTR(err));
	return err;
}

static bool pf_has_valid_vf_bars(struct drm_i915_private *i915)
{
	struct pci_dev *pdev = to_pci_dev(i915->drm.dev);

	if (!i915_pci_resource_valid(pdev, GEN12_VF_GTTMMADR_BAR))
		return false;

	if (HAS_LMEM(i915) && !i915_pci_resource_valid(pdev, GEN12_VF_LMEM_BAR))
		return false;

	return true;
}

static bool pf_continue_as_native(struct drm_i915_private *i915, const char *why)
{
#if IS_ENABLED(CPTCFG_DRM_I915_DEBUG_GEM)
	drm_dbg(&i915->drm, "PF: %s, continuing as native\n", why);
#endif
	pf_reduce_totalvfs(i915, 0);
	return false;
}

static bool pf_verify_readiness(struct drm_i915_private *i915)
{
	struct device *dev = i915->drm.dev;
	struct pci_dev *pdev = to_pci_dev(dev);
	int totalvfs = pci_sriov_get_totalvfs(pdev);
	int newlimit = min_t(u16, wanted_max_vfs(i915), totalvfs);

	GEM_BUG_ON(!dev_is_pf(dev));
	GEM_WARN_ON(totalvfs > U16_MAX);

	if (!newlimit)
		return pf_continue_as_native(i915, "all VFs disabled");

	if (!pf_has_valid_vf_bars(i915))
		return pf_continue_as_native(i915, "VFs BAR not ready");

	if (!works_with_iaf(i915))
		return pf_continue_as_native(i915, "can't work with IAF");

	pf_reduce_totalvfs(i915, newlimit);

	i915->sriov.pf.device_vfs = totalvfs;
	i915->sriov.pf.driver_vfs = newlimit;

	return true;
}

#else

static int pf_reduce_totalvfs(struct drm_i915_private *i915, int limit)
{
	return 0;
}

#endif

/**
 * i915_sriov_probe - Probe I/O Virtualization mode.
 * @i915: the i915 struct
 *
 * This function should be called once and as soon as possible during
 * driver probe to detect whether we are driving a PF or a VF device.
 * SR-IOV PF mode detection is based on PCI @dev_is_pf() function.
 * SR-IOV VF mode detection is based on MMIO register read.
 */
enum i915_iov_mode i915_sriov_probe(struct drm_i915_private *i915)
{
	struct device *dev = i915->drm.dev;
	struct pci_dev *pdev = to_pci_dev(dev);

	if (!HAS_SRIOV(i915))
		return I915_IOV_MODE_NONE;

	if (gen12_pci_capability_is_vf(pdev))
		return I915_IOV_MODE_SRIOV_VF;

#ifdef CONFIG_PCI_IOV
	if (dev_is_pf(dev) && pf_verify_readiness(i915))
		return I915_IOV_MODE_SRIOV_PF;
#endif

	return I915_IOV_MODE_NONE;
}

static void migration_worker_func(struct work_struct *w);

static void vf_init_early(struct drm_i915_private *i915)
{
	INIT_WORK(&i915->sriov.vf.migration_worker, migration_worker_func);
}

static int vf_check_guc_submission_support(struct drm_i915_private *i915)
{
	if (!intel_guc_submission_is_wanted(&to_root_gt(i915)->uc.guc)) {
		drm_err(&i915->drm, "GuC submission disabled\n");
		return -ENODEV;
	}

	return 0;
}

static void vf_tweak_device_info(struct drm_i915_private *i915)
{
	struct intel_device_info *info = mkwrite_device_info(i915);

	/* Force PCH_NOOP. We have no access to display */
	i915->pch_type = PCH_NOP;
	memset(&info->display, 0, sizeof(info->display));
	info->memory_regions &= ~REGION_STOLEN;
}

/**
 * i915_sriov_early_tweaks - Perform early tweaks needed for SR-IOV.
 * @i915: the i915 struct
 *
 * This function should be called once and as soon as possible during
 * driver probe to perform early checks and required tweaks to
 * the driver data.
 */
int i915_sriov_early_tweaks(struct drm_i915_private *i915)
{
	int err;

	if (IS_SRIOV_VF(i915)) {
		vf_init_early(i915);
		err = vf_check_guc_submission_support(i915);
		if (unlikely(err))
			return err;
		vf_tweak_device_info(i915);
	}

	return 0;
}

int i915_sriov_pf_get_device_totalvfs(struct drm_i915_private *i915)
{
	GEM_BUG_ON(!IS_SRIOV_PF(i915));
	return i915->sriov.pf.device_vfs;
}

int i915_sriov_pf_get_totalvfs(struct drm_i915_private *i915)
{
	GEM_BUG_ON(!IS_SRIOV_PF(i915));
	return i915->sriov.pf.driver_vfs;
}

static void pf_set_status(struct drm_i915_private *i915, int status)
{
	GEM_BUG_ON(!IS_SRIOV_PF(i915));
	GEM_BUG_ON(!status);
	GEM_WARN_ON(i915->sriov.pf.__status);

	i915->sriov.pf.__status = status;
}

static bool pf_checklist(struct drm_i915_private *i915)
{
	struct intel_gt *gt;
	unsigned int id;

	GEM_BUG_ON(!IS_SRIOV_PF(i915));

	for_each_gt(gt, i915, id) {
		if (intel_gt_has_unrecoverable_error(gt)) {
			pf_update_status(&gt->iov, -EIO, "GT wedged");
			return false;
		}
	}

	return true;
}

/**
 * i915_sriov_pf_confirm - Confirm that PF is ready to enable VFs.
 * @i915: the i915 struct
 *
 * This function shall be called by the PF when all necessary
 * initialization steps were successfully completed and PF is
 * ready to enable VFs.
 */
void i915_sriov_pf_confirm(struct drm_i915_private *i915)
{
	struct device *dev = i915->drm.dev;
	int totalvfs = i915_sriov_pf_get_totalvfs(i915);
	struct intel_gt *gt;
	unsigned int id;

	GEM_BUG_ON(!IS_SRIOV_PF(i915));

	if (i915_sriov_pf_aborted(i915) || !pf_checklist(i915)) {
		dev_notice(dev, "No VFs could be associated with this PF!\n");
		pf_reduce_totalvfs(i915, 0);
		return;
	}

	dev_info(dev, "%d VFs could be associated with this PF\n", totalvfs);
	pf_set_status(i915, totalvfs);

	/*
	 * FIXME: Temporary solution to force VGT mode in GuC throughout
	 * the life cycle of the PF.
	 */
	for_each_gt(gt, i915, id)
		intel_iov_provisioning_force_vgt_mode(&gt->iov);
}

/**
 * i915_sriov_pf_abort - Abort PF initialization.
 * @i915: the i915 struct
 *
 * This function should be called by the PF when some of the necessary
 * initialization steps failed and PF won't be able to manage VFs.
 */
void i915_sriov_pf_abort(struct drm_i915_private *i915, int err)
{
	GEM_BUG_ON(!IS_SRIOV_PF(i915));
	GEM_BUG_ON(err >= 0);

	__i915_printk(i915, KERN_NOTICE, "PF aborted (%pe) %pS\n",
		      ERR_PTR(err), (void *)_RET_IP_);

	pf_set_status(i915, err);
}

/**
 * i915_sriov_pf_aborted - Check if PF initialization was aborted.
 * @i915: the i915 struct
 *
 * This function may be called by the PF to check if any previous
 * initialization step has failed.
 *
 * Return: true if already aborted
 */
bool i915_sriov_pf_aborted(struct drm_i915_private *i915)
{
	GEM_BUG_ON(!IS_SRIOV_PF(i915));

	return i915->sriov.pf.__status < 0;
}

/**
 * i915_sriov_pf_status - Status of the PF initialization.
 * @i915: the i915 struct
 *
 * This function may be called by the PF to get its status.
 *
 * Return: number of supported VFs if PF is ready or
 *         a negative error code on failure (-EBUSY if
 *         PF initialization is still in progress).
 */
int i915_sriov_pf_status(struct drm_i915_private *i915)
{
	GEM_BUG_ON(!IS_SRIOV_PF(i915));

	return i915->sriov.pf.__status ?: -EBUSY;
}

bool i915_sriov_pf_is_auto_provisioning_enabled(struct drm_i915_private *i915)
{
	GEM_BUG_ON(!IS_SRIOV_PF(i915));

	return !i915->sriov.pf.disable_auto_provisioning;
}

int i915_sriov_pf_set_auto_provisioning(struct drm_i915_private *i915, bool enable)
{
	u16 num_vfs = i915_sriov_pf_get_totalvfs(i915);
	struct intel_gt *gt;
	unsigned int id;
	int err;

	GEM_BUG_ON(!IS_SRIOV_PF(i915));

	if (enable == i915_sriov_pf_is_auto_provisioning_enabled(i915))
		return 0;

	/* disabling is always allowed */
	if (!enable)
		goto set;

	/* enabling is only allowed if all provisioning is empty */
	for_each_gt(gt, i915, id) {
		err = intel_iov_provisioning_verify(&gt->iov, num_vfs);
		if (err == -ENODATA)
			continue;
		return -ESTALE;
	}

set:
	dev_info(i915->drm.dev, "VFs auto-provisioning was turned %s\n",
		 str_on_off(enable));

	i915->sriov.pf.disable_auto_provisioning = !enable;
	return 0;
}

/**
 * i915_sriov_print_info - Print SR-IOV information.
 * @iov: the i915 struct
 * @p: the DRM printer
 *
 * Print SR-IOV related info into provided DRM printer.
 */
void i915_sriov_print_info(struct drm_i915_private *i915, struct drm_printer *p)
{
	struct device *dev = i915->drm.dev;
	struct pci_dev *pdev = to_pci_dev(dev);

	drm_printf(p, "supported: %s\n", str_yes_no(HAS_SRIOV(i915)));
	drm_printf(p, "enabled: %s\n", str_yes_no(IS_SRIOV(i915)));

	if (!IS_SRIOV(i915))
		return;

	drm_printf(p, "mode: %s\n", i915_iov_mode_to_string(IOV_MODE(i915)));

	if (IS_SRIOV_PF(i915)) {
		int status = i915_sriov_pf_status(i915);

		drm_printf(p, "status: %s\n", str_on_off(status > 0));
		if (status < 0)
			drm_printf(p, "error: %d (%pe)\n",
				   status, ERR_PTR(status));

		drm_printf(p, "device vfs: %u\n", i915_sriov_pf_get_device_totalvfs(i915));
		drm_printf(p, "driver vfs: %u\n", i915_sriov_pf_get_totalvfs(i915));
		drm_printf(p, "supported vfs: %u\n", pci_sriov_get_totalvfs(pdev));
		drm_printf(p, "enabled vfs: %u\n", pci_num_vf(pdev));

		/* XXX legacy igt */
		drm_printf(p, "total_vfs: %d\n", pci_sriov_get_totalvfs(pdev));
	}

	/*XXX legacy igt */
	drm_printf(p, "virtualization: %s\n", str_enabled_disabled(true));
}

static int pf_update_guc_clients(struct intel_iov *iov, unsigned int num_vfs)
{
	int err;

	GEM_BUG_ON(!intel_iov_is_pf(iov));

	err = intel_iov_provisioning_push(iov, num_vfs);
	if (unlikely(err))
		IOV_DEBUG(iov, "err=%d", err);

	return err;
}

#ifdef CONFIG_PCI_IOV

#ifndef PCI_EXT_CAP_ID_VF_REBAR
#define PCI_EXT_CAP_ID_VF_REBAR        0x24    /* VF Resizable BAR */
#endif
static void pf_apply_vf_rebar(struct drm_i915_private *i915, unsigned int num_vfs)
{
	struct pci_dev *pdev = to_pci_dev(i915->drm.dev);
	resource_size_t size;
	unsigned int pos;
	u32 rebar, ctrl;
	int i, vf_bar_idx = GEN12_VF_LMEM_BAR - PCI_IOV_RESOURCES;

	if (!HAS_LMEM(i915))
		return;

	pos = pci_find_ext_capability(pdev, PCI_EXT_CAP_ID_VF_REBAR);
	if (!pos)
		return;

	/*
	 * For all current platform we're expecting a single VF resizable BAR, and we expect it to
	 * always be BAR2.
	 */
	pci_read_config_dword(pdev, pos + PCI_REBAR_CTRL, &ctrl);
	if (FIELD_GET(PCI_REBAR_CTRL_NBAR_MASK, ctrl) != 1 ||
	    FIELD_GET(PCI_REBAR_CTRL_BAR_IDX, ctrl) != vf_bar_idx) {
		drm_warn(&i915->drm, "Unexpected resource in VF resizable BAR, skipping resize\n");
		return;
	}

	pci_read_config_dword(pdev, pos + PCI_REBAR_CAP, &rebar);
	rebar = FIELD_GET(PCI_REBAR_CAP_SIZES, rebar);

	while (rebar > 0) {
		i = __fls(rebar);
		size = pci_rebar_size_to_bytes(i);

		if (size * num_vfs <= pci_resource_len(pdev, GEN12_VF_LMEM_BAR)) {
			ctrl &= ~PCI_REBAR_CTRL_BAR_SIZE;
			ctrl |= pci_rebar_bytes_to_size(size) << PCI_REBAR_CTRL_BAR_SHIFT;
			pci_write_config_dword(pdev, pos + PCI_REBAR_CTRL, ctrl);
			pdev->sriov->barsz[vf_bar_idx] = size;
			drm_info(&i915->drm, "VF BAR%d resized to %dM\n", vf_bar_idx, 1 << i);

			break;
		}

		rebar &= ~BIT(i);
	}
}

#else

static void pf_apply_vf_rebar(struct drm_i915_private *i915, unsigned int num_vfs)
{
}

#endif

/**
 * i915_sriov_pf_enable_vfs - Enable VFs.
 * @i915: the i915 struct
 * @num_vfs: number of VFs to enable (shall not be zero)
 *
 * This function will enable specified number of VFs. Note that VFs can be
 * enabled only after successful PF initialization.
 * This function shall be called only on PF.
 *
 * Return: number of configured VFs or a negative error code on failure.
 */
int i915_sriov_pf_enable_vfs(struct drm_i915_private *i915, int num_vfs)
{
	bool auto_provisioning = i915_sriov_pf_is_auto_provisioning_enabled(i915);
	struct device *dev = i915->drm.dev;
	struct pci_dev *pdev = to_pci_dev(dev);
	struct intel_gt *gt;
	unsigned int id;
	int err;

	GEM_BUG_ON(!IS_SRIOV_PF(i915));
	GEM_BUG_ON(num_vfs < 0);
	drm_dbg(&i915->drm, "enabling %d VFs\n", num_vfs);

	/* verify that all initialization was successfully completed */
	err = i915_sriov_pf_status(i915);
	if (err < 0)
		goto fail;

	/* ensure the debugger is disabled */
	err = i915_debugger_disallow(i915);
	if (err < 0)
		goto fail;

	/* hold the reference to runtime pm as long as VFs are enabled */
	for_each_gt(gt, i915, id)
		intel_gt_pm_get_untracked(gt);

	/* Wa:16014207253 */
	for_each_gt(gt, i915, id)
		intel_boost_fake_int_timer(gt, true);

	/* Wa:16015666671 & Wa:16015476723 */
	pvc_wa_disallow_rc6(i915);

	for_each_gt(gt, i915, id) {
		err = intel_iov_provisioning_verify(&gt->iov, num_vfs);
		if (err == -ENODATA) {
			if (auto_provisioning)
				err = intel_iov_provisioning_auto(&gt->iov, num_vfs);
			else
				err = 0; /* trust late provisioning */
		}
		if (unlikely(err))
			goto fail_pm;

		/*
		 * Update cached values of runtime registers shared with the VFs in case
		 * HuC status register has been updated by the GSC after our initial probe.
		 */
		if (intel_uc_wants_huc(&gt->uc) && intel_huc_is_loaded_by_gsc(&gt->uc.huc)) {
			intel_iov_service_update(&gt->iov);
		}
	}

	for_each_gt(gt, i915, id) {
		err = pf_update_guc_clients(&gt->iov, num_vfs);
		if (unlikely(err < 0))
			goto fail_pm;
	}

	pf_apply_vf_rebar(i915, num_vfs);

	err = pci_enable_sriov(pdev, num_vfs);
	if (err < 0)
		goto fail_guc;

	i915_sriov_sysfs_update_links(i915, true);

	dev_info(dev, "Enabled %u VFs\n", num_vfs);
	return num_vfs;

fail_guc:
	for_each_gt(gt, i915, id)
		pf_update_guc_clients(&gt->iov, 0);
fail_pm:
	for_each_gt(gt, i915, id) {
		intel_iov_provisioning_auto(&gt->iov, 0);
		intel_boost_fake_int_timer(gt, false);
	}
	pvc_wa_allow_rc6(i915);
	for_each_gt(gt, i915, id)
		intel_gt_pm_put_untracked(gt);
	i915_debugger_allow(i915);
fail:
	drm_err(&i915->drm, "Failed to enable %u VFs (%pe)\n",
		num_vfs, ERR_PTR(err));
	return err;
}

static void pf_start_vfs_flr(struct intel_iov *iov, unsigned int num_vfs)
{
	unsigned int n;

	GEM_BUG_ON(!intel_iov_is_pf(iov));

	for (n = 1; n <= num_vfs; n++)
		intel_iov_state_start_flr(iov, n);
}

#define I915_VF_FLR_TIMEOUT_MS 500UL

static void pf_wait_vfs_flr(struct intel_iov *iov, unsigned int num_vfs)
{
	unsigned long timeout_ms = I915_VF_FLR_TIMEOUT_MS;
	unsigned int n;

	GEM_BUG_ON(!intel_iov_is_pf(iov));

	for (n = 1; n <= num_vfs; n++) {
		if (wait_for(intel_iov_state_no_flr(iov, n), timeout_ms)) {
			IOV_ERROR(iov, "VF%u FLR didn't complete within %lu ms\n",
				  n, timeout_ms);
			timeout_ms /= 2;
		}
	}
}

/**
 * i915_sriov_pf_disable_vfs - Disable VFs.
 * @i915: the i915 struct
 *
 * This function will disable all previously enabled VFs.
 * This function shall be called only on PF.
 *
 * Return: 0 on success or a negative error code on failure.
 */
int i915_sriov_pf_disable_vfs(struct drm_i915_private *i915)
{
	struct device *dev = i915->drm.dev;
	struct pci_dev *pdev = to_pci_dev(dev);
	u16 num_vfs = pci_num_vf(pdev);
	u16 vfs_assigned = pci_vfs_assigned(pdev);
	struct intel_gt *gt;
	unsigned int id;

	GEM_BUG_ON(!IS_SRIOV_PF(i915));
	drm_dbg(&i915->drm, "disabling %u VFs\n", num_vfs);

	if (vfs_assigned) {
		dev_warn(dev, "Can't disable %u VFs, %u are still assigned\n",
			 num_vfs, vfs_assigned);
		return -EPERM;
	}

	if (!num_vfs)
		return 0;

	i915_sriov_sysfs_update_links(i915, false);

	pci_disable_sriov(pdev);

	for_each_gt(gt, i915, id)
		pf_start_vfs_flr(&gt->iov, num_vfs);
	for_each_gt(gt, i915, id)
		pf_wait_vfs_flr(&gt->iov, num_vfs);

	for_each_gt(gt, i915, id) {
		pf_update_guc_clients(&gt->iov, 0);
		intel_iov_provisioning_auto(&gt->iov, 0);
	}

	/* Wa:16015666671 & Wa:16015476723 */
	pvc_wa_allow_rc6(i915);

	/* Wa:16014207253 */
	for_each_gt(gt, i915, id)
		intel_boost_fake_int_timer(gt, false);

	for_each_gt(gt, i915, id)
		intel_gt_pm_put_untracked(gt);

	i915_debugger_allow(i915);

	dev_info(dev, "Disabled %u VFs\n", num_vfs);
	return 0;
}

static bool needs_save_restore(struct drm_i915_private *i915, unsigned int vfid)
{
	struct pci_dev *pdev = to_pci_dev(i915->drm.dev);
	struct pci_dev *vfpdev = i915_pci_pf_get_vf_dev(pdev, vfid);
	bool ret;

	if (!vfpdev)
		return false;

	/*
	 * If VF has the same driver as PF loaded (from host perspective), we don't need
	 * to save/restore its state, because the VF driver will receive the same PM
	 * handling as all the host drivers. There is also no need to save/restore state
	 * when no driver is loaded on VF.
	 */
	ret = (vfpdev->driver && strcmp(vfpdev->driver->name, pdev->driver->name) != 0);

	pci_dev_put(vfpdev);
	return ret;
}

static void pf_restore_vfs_pci_state(struct drm_i915_private *i915, unsigned int num_vfs)
{
	struct pci_dev *pdev = to_pci_dev(i915->drm.dev);
	unsigned int vfid;

	GEM_BUG_ON(num_vfs > pci_num_vf(pdev));

	for (vfid = 1; vfid <= num_vfs; vfid++) {
		struct pci_dev *vfpdev = i915_pci_pf_get_vf_dev(pdev, vfid);

		if (!vfpdev)
			continue;
		if (!needs_save_restore(i915, vfid))
			continue;

		/*
		 * XXX: Waiting for other drivers to do their job.
		 * We can ignore the potential error in this function -
		 * in case of an error, we still want to try to reinitialize
		 * the MSI and set the PCI master.
		 */
		device_pm_wait_for_dev(&pdev->dev, &vfpdev->dev);

		pci_restore_msi_state(vfpdev);
		pci_set_master(vfpdev);

		pci_dev_put(vfpdev);
	}
}

#define I915_VF_PAUSE_TIMEOUT_MS 500
#define I915_VF_REPROVISION_TIMEOUT_MS 1000

static int pf_gt_save_vf_guc_state(struct intel_gt *gt, unsigned int vfid)
{
	struct pci_dev *pdev = to_pci_dev(gt->i915->drm.dev);
	struct intel_iov *iov = &gt->iov;
	unsigned long timeout_ms = I915_VF_PAUSE_TIMEOUT_MS;
	void **guc_state = &iov->pf.state.data[vfid].guc_state;
	int err;

	GEM_BUG_ON(!vfid);
	GEM_BUG_ON(vfid > pci_num_vf(pdev));

	err = intel_iov_state_pause_vf(iov, vfid);
	if (err) {
		IOV_ERROR(iov, "Failed to pause VF%u: (%pe)", vfid, ERR_PTR(err));
		return err;
	}

	/* FIXME: How long we should wait? */
	if (wait_for(iov->pf.state.data[vfid].paused, timeout_ms)) {
		IOV_ERROR(iov, "VF%u pause didn't complete within %lu ms\n", vfid, timeout_ms);
		return -ETIMEDOUT;
	}

	if (*guc_state)
#ifdef BPM_VFIO_SR_IOV_VF_MIGRATION_NOT_PRESENT
		memset(*guc_state, 0, SZ_4K);
#else
		memset(*guc_state, 0, PF2GUC_SAVE_RESTORE_VF_BUFF_SIZE);
#endif
	else
#ifdef BPM_VFIO_SR_IOV_VF_MIGRATION_NOT_PRESENT
		*guc_state = kzalloc(SZ_4K, GFP_KERNEL);
#else
		*guc_state = kzalloc(PF2GUC_SAVE_RESTORE_VF_BUFF_SIZE, GFP_KERNEL);
#endif

	if (!*guc_state) {
		err = -ENOMEM;
		goto error;
	}

#ifdef BPM_VFIO_SR_IOV_VF_MIGRATION_NOT_PRESENT
	err = intel_iov_state_save_vf(iov, vfid, *guc_state);
#else
	err = intel_iov_state_save_vf(iov, vfid, *guc_state, PF2GUC_SAVE_RESTORE_VF_BUFF_SIZE);
#endif
error:
	if (err)
		IOV_ERROR(iov, "Failed to save VF%u GuC state: (%pe)", vfid, ERR_PTR(err));

	return err;
}

static void pf_save_vfs_guc_state(struct drm_i915_private *i915, unsigned int num_vfs)
{
	unsigned int saved = 0;
	struct intel_gt *gt;
	unsigned int gt_id;
	unsigned int vfid;

	for (vfid = 1; vfid <= num_vfs; vfid++) {
		if (!needs_save_restore(i915, vfid)) {
			drm_dbg(&i915->drm, "Save of VF%u GuC state has been skipped\n", vfid);
			continue;
		}

		for_each_gt(gt, i915, gt_id) {
			int err = pf_gt_save_vf_guc_state(gt, vfid);

			if (err < 0)
				goto skip_vf;
		}
		saved++;
		continue;
skip_vf:
		break;
	}

	drm_dbg(&i915->drm, "%u of %u VFs GuC state successfully saved", saved, num_vfs);
}

static int pf_gt_restore_vf_guc_state(struct intel_gt *gt, unsigned int vfid)
{
	struct pci_dev *pdev = to_pci_dev(gt->i915->drm.dev);
	struct intel_iov *iov = &gt->iov;
	unsigned long timeout_ms = I915_VF_REPROVISION_TIMEOUT_MS;
	int err;

	GEM_BUG_ON(!vfid);
	GEM_BUG_ON(vfid > pci_num_vf(pdev));

	if (!iov->pf.state.data[vfid].guc_state)
		return -EINVAL;

	if (wait_for(iov->pf.provisioning.num_pushed >= vfid, timeout_ms)) {
		IOV_ERROR(iov,
			  "Failed to restore VF%u GuC state. Provisioning didn't complete within %lu ms\n",
			  vfid, timeout_ms);
		return -ETIMEDOUT;
	}

#ifdef BPM_VFIO_SR_IOV_VF_MIGRATION_NOT_PRESENT
	err = intel_iov_state_restore_vf(iov, vfid, iov->pf.state.data[vfid].guc_state);
#else
	err = intel_iov_state_restore_vf(iov, vfid, iov->pf.state.data[vfid].guc_state,
					 PF2GUC_SAVE_RESTORE_VF_BUFF_SIZE);
#endif
	if (err < 0) {
		IOV_ERROR(iov, "Failed to restore VF%u GuC state: (%pe)", vfid, ERR_PTR(err));
		return err;
	}

	kfree(iov->pf.state.data[vfid].guc_state);
	iov->pf.state.data[vfid].guc_state = NULL;

	return 0;
}

static void pf_restore_vfs_guc_state(struct drm_i915_private *i915, unsigned int num_vfs)
{
	unsigned int restored = 0;
	struct intel_gt *gt;
	unsigned int gt_id;
	unsigned int vfid;

	for (vfid = 1; vfid <= num_vfs; vfid++) {
		if (!needs_save_restore(i915, vfid)) {
			drm_dbg(&i915->drm, "Restoration of VF%u GuC state has been skipped\n",
				vfid);
			continue;
		}

		for_each_gt(gt, i915, gt_id) {
			int err = pf_gt_restore_vf_guc_state(gt, vfid);

			if (err < 0)
				goto skip_vf;
		}
		restored++;
		continue;
skip_vf:
		break;
	}

	drm_dbg(&i915->drm, "%u of %u VFs GuC state restored successfully", restored, num_vfs);
}

static i915_reg_t vf_master_irq(struct drm_i915_private *i915, unsigned int vfid)
{
	return (GRAPHICS_VER_FULL(i915) < IP_VER(12, 50)) ?
		GEN12_VF_GFX_MSTR_IRQ(vfid) :
		XEHPSDV_VF_GFX_MSTR_IRQ(vfid);
}

static void pf_restore_vfs_irqs(struct drm_i915_private *i915, unsigned int num_vfs)
{
	struct intel_gt *gt;
	unsigned int gt_id;

	for_each_gt(gt, i915, gt_id) {
		unsigned int vfid;

		for (vfid = 1; vfid <= num_vfs; vfid++)
			raw_reg_write(gt->uncore->regs, vf_master_irq(i915, vfid),
				      GEN11_MASTER_IRQ);
	}
}

static void pf_suspend_active_vfs(struct drm_i915_private *i915)
{
	struct pci_dev *pdev = to_pci_dev(i915->drm.dev);
	unsigned int num_vfs = pci_num_vf(pdev);

	GEM_BUG_ON(!IS_SRIOV_PF(i915));

	if (num_vfs == 0)
		return;

	pf_save_vfs_guc_state(i915, num_vfs);
}

static void pf_resume_active_vfs(struct drm_i915_private *i915)
{
	struct pci_dev *pdev = to_pci_dev(i915->drm.dev);
	unsigned int num_vfs = pci_num_vf(pdev);

	GEM_BUG_ON(!IS_SRIOV_PF(i915));

	if (num_vfs == 0)
		return;

	pf_restore_vfs_pci_state(i915, num_vfs);
	pf_restore_vfs_guc_state(i915, num_vfs);
	pf_restore_vfs_irqs(i915, num_vfs);
}

/**
 * i915_sriov_suspend_prepare - Prepare SR-IOV to suspend.
 * @i915: the i915 struct
 *
 * The function is called in a callback prepare.
 *
 * Return: 0 on success or a negative error code on failure.
 */
int i915_sriov_suspend_prepare(struct drm_i915_private *i915)
{
	if (IS_SRIOV_PF(i915))
		pf_suspend_active_vfs(i915);

	return 0;
}

/**
 * i915_sriov_pf_stop_vf - Stop VF.
 * @i915: the i915 struct
 * @vfid: VF identifier
 *
 * This function will stop VF on all tiles.
 * This function shall be called only on PF.
 *
 * Return: 0 on success or a negative error code on failure.
 */
int i915_sriov_pf_stop_vf(struct drm_i915_private *i915, unsigned int vfid)
{
	struct device *dev = i915->drm.dev;
	struct intel_gt *gt;
	unsigned int id;
	int result = 0;
	int err;

	GEM_BUG_ON(!IS_SRIOV_PF(i915));
	for_each_gt(gt, i915, id) {
		err = intel_iov_state_stop_vf(&gt->iov, vfid);
		if (unlikely(err)) {
			dev_warn(dev, "Failed to stop VF%u on gt%u (%pe)\n",
				 vfid, id, ERR_PTR(err));
			result = result ?: err;
		}
	}

	return result;
}

/**
 * i915_sriov_pf_pause_vf - Pause VF.
 * @i915: the i915 struct
 * @vfid: VF identifier
 *
 * This function will pause VF on all tiles.
 * This function shall be called only on PF.
 *
 * Return: 0 on success or a negative error code on failure.
 */
int i915_sriov_pf_pause_vf(struct drm_i915_private *i915, unsigned int vfid)
{
	struct device *dev = i915->drm.dev;
	struct intel_gt *gt;
	unsigned int id;
	int result = 0;
	int err;

	GEM_BUG_ON(!IS_SRIOV_PF(i915));
	for_each_gt(gt, i915, id) {
		err = intel_iov_state_pause_vf(&gt->iov, vfid);
		if (unlikely(err)) {
			dev_warn(dev, "Failed to pause VF%u on gt%u (%pe)\n",
				 vfid, id, ERR_PTR(err));
			result = result ?: err;
		}
	}

	return result;
}

/**
 * i915_sriov_pf_resume_vf - Resume VF.
 * @i915: the i915 struct
 * @vfid: VF identifier
 *
 * This function will resume VF on all tiles.
 * This function shall be called only on PF.
 *
 * Return: 0 on success or a negative error code on failure.
 */
int i915_sriov_pf_resume_vf(struct drm_i915_private *i915, unsigned int vfid)
{
	struct device *dev = i915->drm.dev;
	struct intel_gt *gt;
	unsigned int id;
	int result = 0;
	int err;

	GEM_BUG_ON(!IS_SRIOV_PF(i915));
	for_each_gt(gt, i915, id) {
		err = intel_iov_state_resume_vf(&gt->iov, vfid);
		if (unlikely(err)) {
			dev_warn(dev, "Failed to resume VF%u on gt%u (%pe)\n",
				 vfid, id, ERR_PTR(err));
			result = result ?: err;
		}
	}

	return result;
}

/**
 * i915_sriov_pause_vf - Pause VF.
 * @pdev: the i915 struct
 * @vfid: VF identifier
 *
 * This function will pause VF on all tiles.
 * This function shall be called only on PF.
 *
 * Return: 0 on success or a negative error code on failure.
 */
int i915_sriov_pause_vf(struct pci_dev *pdev, unsigned int vfid)
{
	struct drm_i915_private *i915 = pci_get_drvdata(pdev);

	if (!IS_SRIOV_PF(i915))
		return -ENODEV;

	return i915_sriov_pf_pause_vf(i915, vfid);
}
#ifndef BPM_VFIO_SR_IOV_VF_MIGRATION_NOT_PRESENT
EXPORT_SYMBOL_NS_GPL(i915_sriov_pause_vf, I915);
#endif

/**
 * i915_sriov_resume_vf - Resume VF.
 * @pdev: the i915 struct
 * @vfid: VF identifier
 *
 * This function will resume VF on all tiles.
 * This function shall be called only on PF.
 *
 * Return: 0 on success or a negative error code on failure.
 */
int i915_sriov_resume_vf(struct pci_dev *pdev, unsigned int vfid)
{
	struct drm_i915_private *i915 = pci_get_drvdata(pdev);

	if (!IS_SRIOV_PF(i915))
		return -ENODEV;

	return i915_sriov_pf_resume_vf(i915, vfid);
}
#ifndef BPM_VFIO_SR_IOV_VF_MIGRATION_NOT_PRESENT
EXPORT_SYMBOL_NS_GPL(i915_sriov_resume_vf, I915);
#endif

/**
 * i915_sriov_wait_vf_flr_done - Wait for VF FLR completion.
 * @pdev: PF pci device
 * @vfid: VF identifier
 *
 * This function will wait until VF FLR is processed by PF on all tiles (or
 * until timeout occurs).
 * This function shall be called only on PF.
 *
 * Return: 0 on success or a negative error code on failure.
 */
int i915_sriov_wait_vf_flr_done(struct pci_dev *pdev, unsigned int vfid)
{
	struct drm_i915_private *i915 = pci_get_drvdata(pdev);
	struct intel_gt *gt;
	unsigned int id;
	int ret;

	if (!IS_SRIOV_PF(i915))
		return -ENODEV;

	for_each_gt(gt, i915, id) {
		ret = wait_for(intel_iov_state_no_flr(&gt->iov, vfid), I915_VF_FLR_TIMEOUT_MS);
		if (ret)
			return ret;
	}

	return 0;
}
#ifndef BPM_VFIO_SR_IOV_VF_MIGRATION_NOT_PRESENT
EXPORT_SYMBOL_NS_GPL(i915_sriov_wait_vf_flr_done, I915);

static struct intel_gt *
sriov_to_gt(struct pci_dev *pdev, unsigned int tile, bool standalone)
{
	struct drm_i915_private *i915 = pci_get_drvdata(pdev);
	struct intel_gt *gt;

	if (!i915 || !IS_SRIOV_PF(i915))
		return NULL;

	if (!standalone && !HAS_REMOTE_TILES(i915) && tile > 0)
               return NULL;

	gt = NULL;
	if (tile < ARRAY_SIZE(i915->gt))
		gt = i915->gt[tile];

	return gt;
}

/**
 * i915_sriov_ggtt_size - Get size needed to store VF GGTT.
 * @pdev: PF pci device
 * @vfid: VF identifier
 * @tile: tile identifier
 *
 * This function shall be called only on PF.
 *
 * Return: Size in bytes.
 */
size_t
i915_sriov_ggtt_size(struct pci_dev *pdev, unsigned int vfid, unsigned int tile)
{
	struct intel_gt *gt;
	ssize_t size;

	gt = sriov_to_gt(pdev, tile, false);
	if (!gt)
		return 0;

	size = intel_iov_state_save_ggtt(&gt->iov, vfid, NULL, 0);
	WARN_ON(size < 0);

	return size;
}
EXPORT_SYMBOL_NS_GPL(i915_sriov_ggtt_size, I915);

/**
 * i915_sriov_ggtt_save - Save VF GGTT.
 * @pdev: PF pci device
 * @vfid: VF identifier
 * @tile: tile identifier
 * @buf: buffer to save VF GGTT
 * @size: size of buffer to save VF GGTT
 *
 * This function shall be called only on PF.
 *
 * Return: Size of data written on success or a negative error code on failure.
 */
ssize_t i915_sriov_ggtt_save(struct pci_dev *pdev, unsigned int vfid, unsigned int tile,
			     void *buf, size_t size)
{
	struct intel_gt *gt;

	gt = sriov_to_gt(pdev, tile, true);
	if (!gt)
		return -ENODEV;

	WARN_ON(buf == NULL && size == 0);

	return intel_iov_state_save_ggtt(&gt->iov, vfid, buf, size);
}
EXPORT_SYMBOL_NS_GPL(i915_sriov_ggtt_save, I915);

/**
 * i915_sriov_ggtt_load - Load VF GGTT.
 * @pdev: PF pci device
 * @vfid: VF identifier
 * @tile: tile identifier
 * @buf: buffer with VF GGTT
 * @size: size of buffer with VF GGTT
 *
 * This function shall be called only on PF.
 *
 * Return: 0 on success or a negative error code on failure.
 */
int
i915_sriov_ggtt_load(struct pci_dev *pdev, unsigned int vfid, unsigned int tile,
		     const void *buf, size_t size)
{
	struct intel_gt *gt;

	gt = sriov_to_gt(pdev, tile, true);
	if (!gt)
		return -ENODEV;

	return intel_iov_state_restore_ggtt(&gt->iov, vfid, buf, size);
}
EXPORT_SYMBOL_NS_GPL(i915_sriov_ggtt_load, I915);

/**
 * i915_sriov_lmem_size - Get size needed to store VF Local Memory.
 * @pdev: PF pci device
 * @vfid: VF identifier
 * @tile: tile identifier
 *
 * This function shall be called only on PF.
 *
 * Return: Size in bytes.
 */
size_t
i915_sriov_lmem_size(struct pci_dev *pdev, unsigned int vfid, unsigned int tile)
{
	struct intel_gt *gt;

	gt = sriov_to_gt(pdev, tile, false);
	if (!gt)
		return 0;

	return intel_iov_provisioning_get_lmem(&gt->iov, vfid);
}
EXPORT_SYMBOL_NS_GPL(i915_sriov_lmem_size, I915);

/**
 * i915_sriov_lmem_map - Map VF LMEM.
 * @pdev: PF pci device
 * @vfid: VF identifier
 * @tile: tile identifier
 *
 * Return: Pointer to VF LMEM or NULL on failure.
 *
 * This function shall be called only on PF.
 */
void *i915_sriov_lmem_map(struct pci_dev *pdev, unsigned int vfid, unsigned int tile)
{
	struct intel_gt *gt;

	gt = sriov_to_gt(pdev, tile, true);
	if (!gt)
		return NULL;

	return intel_iov_state_map_lmem(&gt->iov, vfid);
}
EXPORT_SYMBOL_NS_GPL(i915_sriov_lmem_map, I915);

/**
 * i915_sriov_lmem_unmap - Unmap VF LMEM.
 * @pdev: PF pci device
 * @vfid: VF identifier
 * @tile: tile identifier
 *
 * This function shall be called only on PF.
 */
void
i915_sriov_lmem_unmap(struct pci_dev *pdev, unsigned int vfid, unsigned int tile)
{
	struct intel_gt *gt;

	gt = sriov_to_gt(pdev, tile, true);
	if (!gt)
		return;

	return intel_iov_state_unmap_lmem(&gt->iov, vfid);
}
EXPORT_SYMBOL_NS_GPL(i915_sriov_lmem_unmap, I915);

/**
 * i915_sriov_fw_state_size - Get size needed to store GuC FW state.
 * @pdev: PF pci device
 * @vfid: VF identifier
 * @tile: tile identifier
 *
 * This function shall be called only on PF.
 *
 * Return: Size in bytes.
 */
size_t
i915_sriov_fw_state_size(struct pci_dev *pdev, unsigned int vfid, unsigned int tile)
{
	struct intel_gt *gt;

	gt = sriov_to_gt(pdev, tile, true);
	if (!gt)
		return 0;

	return SZ_4K;
}
EXPORT_SYMBOL_NS_GPL(i915_sriov_fw_state_size, I915);

/**
 * i915_sriov_fw_state_save - Save GuC FW state.
 * @pdev: PF pci device
 * @vfid: VF identifier
 * @tile: tile identifier
 * @buf: buffer to save GuC FW state
 * @size: size of buffer to save GuC FW state
 *
 * This function shall be called only on PF.
 *
 * Return: Size of data written on success or a negative error code on failure.
 */
ssize_t
i915_sriov_fw_state_save(struct pci_dev *pdev, unsigned int vfid, unsigned int tile,
			 void *buf, size_t size)
{
	struct intel_gt *gt;
	int ret;

	gt = sriov_to_gt(pdev, tile, true);
	if (!gt)
		return -ENODEV;

	ret = intel_iov_state_save_vf(&gt->iov, vfid, buf, size);
	if (ret)
		return ret;

	return SZ_4K;
}
EXPORT_SYMBOL_NS_GPL(i915_sriov_fw_state_save, I915);

/**
 * i915_sriov_fw_state_load - Load GuC FW state.
 * @pdev: PF pci device
 * @vfid: VF identifier
 * @tile: tile identifier
 * @buf: buffer with GuC FW state to load
 * @size: size of buffer with GuC FW state
 *
 * This function shall be called only on PF.
 *
 * Return: 0 on success or a negative error code on failure.
 */
int
i915_sriov_fw_state_load(struct pci_dev *pdev, unsigned int vfid, unsigned int tile,
			 const void *buf, size_t size)
{
	struct intel_gt *gt;

	gt = sriov_to_gt(pdev, tile, true);
	if (!gt)
		return -ENODEV;

	return intel_iov_state_store_guc_migration_state(&gt->iov, vfid, buf, size);
}
EXPORT_SYMBOL_NS_GPL(i915_sriov_fw_state_load, I915);
#endif /* BPM_VFIO_SR_IOV_VF_MIGRATION_NOT_PRESENT */

/**
 * i915_sriov_pf_clear_vf - Unprovision VF.
 * @i915: the i915 struct
 * @vfid: VF identifier
 *
 * This function will uprovision VF on all tiles.
 * This function shall be called only on PF.
 *
 * Return: 0 on success or a negative error code on failure.
 */
int i915_sriov_pf_clear_vf(struct drm_i915_private *i915, unsigned int vfid)
{
	struct device *dev = i915->drm.dev;
	struct intel_gt *gt;
	unsigned int id;
	int result = 0;
	int err;

	GEM_BUG_ON(!IS_SRIOV_PF(i915));
	for_each_gt(gt, i915, id) {
		err = intel_iov_provisioning_clear(&gt->iov, vfid);
		if (unlikely(err)) {
			dev_warn(dev, "Failed to unprovision VF%u on gt%u (%pe)\n",
				 vfid, id, ERR_PTR(err));
			result = result ?: err;
		}
	}

	return result;
}

/**
 * i915_sriov_suspend_late - Suspend late SR-IOV.
 * @i915: the i915 struct
 *
 * The function is called in a callback suspend_late.
 *
 * Return: 0 on success or a negative error code on failure.
 */
int i915_sriov_suspend_late(struct drm_i915_private *i915)
{
	struct intel_gt *gt;
	unsigned int id;

	if (IS_SRIOV_PF(i915)) {
		/*
		 * When we're enabling the VFs in i915_sriov_pf_enable_vfs(), we also get
		 * a GT PM wakeref which we hold for the whole VFs life cycle.
		 * However for the time of suspend this wakeref must be put back.
		 * We'll get it back during the resume in i915_sriov_resume_early().
		 */
		if (pci_num_vf(to_pci_dev(i915->drm.dev)) != 0) {
			for_each_gt(gt, i915, id)
				intel_gt_pm_put_untracked(gt);
		}
	}

	return 0;
}

/**
 * i915_sriov_resume_early - Resume early SR-IOV.
 * @i915: the i915 struct
 *
 * The function is called in a callback resume_early.
 *
 * Return: 0 on success or a negative error code on failure.
 */
int i915_sriov_resume_early(struct drm_i915_private *i915)
{
	struct intel_gt *gt;
	unsigned int id;

	if (IS_SRIOV_PF(i915)) {
		/*
		 * When we're enabling the VFs in i915_sriov_pf_enable_vfs(), we also get
		 * a GT PM wakeref which we hold for the whole VFs life cycle.
		 * However for the time of suspend this wakeref must be put back.
		 * If we have VFs enabled, now is the moment at which we get back this wakeref.
		 */
		if (pci_num_vf(to_pci_dev(i915->drm.dev)) != 0) {
			for_each_gt(gt, i915, id)
				intel_gt_pm_get_untracked(gt);
		}
	}

	return 0;
}

/**
 * i915_sriov_resume - Resume SR-IOV.
 * @i915: the i915 struct
 *
 * The function is called in a callback resume.
 *
 * Return: 0 on success or a negative error code on failure.
 */
int i915_sriov_resume(struct drm_i915_private *i915)
{
	if (IS_SRIOV_PF(i915))
		pf_resume_active_vfs(i915);

	return 0;
}

static void intel_gt_default_contexts_ring_restore(struct intel_gt *gt)
{
	struct intel_engine_cs *engine;
	enum intel_engine_id eid;

	for_each_engine(engine, gt, eid) {
		struct intel_context *ce;

		list_for_each_entry(ce, &engine->pinned_contexts_list,
				    pinned_contexts_link)
			intel_context_revert_ring_heads(ce);
	}
}

static void default_contexts_ring_restore(struct drm_i915_private *i915)
{
	struct intel_gt *gt;
	unsigned int id;

	for_each_gt(gt, i915, id) {
		intel_gt_default_contexts_ring_restore(gt);
	}
}

static void user_contexts_ring_restore(struct drm_i915_private *i915)
{
	struct i915_gem_context *ctx;

	spin_lock_irq(&i915->gem.contexts.lock);
	rcu_read_lock();
	list_for_each_entry_rcu(ctx, &i915->gem.contexts.list, link) {
		struct i915_gem_engines_iter it;
		struct intel_context *ce;

		if (!kref_get_unless_zero(&ctx->ref))
			continue;

		for_each_gem_engine(ce, rcu_dereference(ctx->engines), it) {
			intel_context_revert_ring_heads(ce);
		}

		i915_gem_context_put(ctx);
	}
	rcu_read_unlock();
	spin_unlock_irq(&i915->gem.contexts.lock);
}

static void user_contexts_hwsp_rebase(struct drm_i915_private *i915)
{
	struct i915_gem_context *ctx;

	spin_lock_irq(&i915->gem.contexts.lock);
	rcu_read_lock();
	list_for_each_entry_rcu(ctx, &i915->gem.contexts.list, link) {
		struct i915_gem_engines_iter it;
		struct intel_context *ce;

		if (!kref_get_unless_zero(&ctx->ref))
			continue;
		spin_unlock_irq(&i915->gem.contexts.lock);

		for_each_gem_engine(ce, rcu_dereference(ctx->engines), it)
			intel_context_rebase_hwsp(ce);

		spin_lock_irq(&i915->gem.contexts.lock);
		i915_gem_context_put(ctx);
	}
	rcu_read_unlock();
	spin_unlock_irq(&i915->gem.contexts.lock);
}

static void intel_gt_default_contexts_hwsp_rebase(struct intel_gt *gt)
{
	struct intel_engine_cs *engine;
	enum intel_engine_id eid;

	for_each_engine(engine, gt, eid) {
		struct intel_context *ce;

		list_for_each_entry(ce, &engine->pinned_contexts_list,
				    pinned_contexts_link)
			intel_context_rebase_hwsp(ce);
	}
}

static void default_contexts_hwsp_rebase(struct drm_i915_private *i915)
{
	struct intel_gt *gt;
	unsigned int id;

	for_each_gt(gt, i915, id)
		intel_gt_default_contexts_hwsp_rebase(gt);
}

static void vf_post_migration_fixup_contexts(struct drm_i915_private *i915)
{
	default_contexts_hwsp_rebase(i915);
	user_contexts_hwsp_rebase(i915);
	default_contexts_ring_restore(i915);
	user_contexts_ring_restore(i915);
}

static void heartbeats_disable(struct drm_i915_private *i915)
{
	struct intel_gt *gt;
	unsigned int id;

	for_each_gt(gt, i915, id) {
		intel_gt_heartbeats_disable(gt);
	}
}

static void heartbeats_restore(struct drm_i915_private *i915, bool unpark)
{
	struct intel_gt *gt;
	unsigned int id;

	for_each_gt(gt, i915, id) {
		intel_gt_heartbeats_restore(gt, unpark);
	}
}

/**
 * submissions_disable - Turn off advancing with execution of scheduled submissions.
 * @i915: the i915 struct
 *
 * When the hardware is not ready to accept submissions, continuing to push
 * the scheduled requests would only lead to a series of errors, and aborting
 * requests which could be successfully executed if submitted after the pipeline
 * is back to ready state.
 */
static void submissions_disable(struct drm_i915_private *i915)
{
	struct intel_gt *gt;
	unsigned int id;

	for_each_gt(gt, i915, id) {
		intel_guc_submission_pause(&gt->uc.guc);
	}
}

/**
 * submissions_restore - Re-enable advancing with execution of scheduled submissions.
 * @i915: the i915 struct
 *
 * We possibly unwinded some requests which did not finished before migration; now
 * we can allow these requests to be re-submitted.
 */
static void submissions_restore(struct drm_i915_private *i915)
{
	struct intel_gt *gt;
	unsigned int id;

	for_each_gt(gt, i915, id) {
		intel_guc_submission_restore(&gt->uc.guc);
	}
}

/**
 * vf_post_migration_status_page_sanitization_disable - Disable sanitization of status pages.
 * @i915: the i915 struct
 *
 * The post-migration recovery uses gt sanitization code to prepare the driver re-enabling.
 * This code however clears status pages of contexts, as it assumes they were damaged
 * by suspend. Migration is not suspend, and it keeps the status pages content intact.
 * In fact, we need the values within to recover unfinished submissions.
 */
static void vf_post_migration_status_page_sanitization_disable(struct drm_i915_private *i915)
{
	struct intel_gt *gt;
	unsigned int id;

	for_each_gt(gt, i915, id)
		guc_submission_status_page_sanitization_disable(&gt->uc.guc);
}

/**
 * vf_post_migration_status_page_sanitization_enable - Re-enable sanitization of status pages.
 * @i915: the i915 struct
 */
static void vf_post_migration_status_page_sanitization_enable(struct drm_i915_private *i915)
{
	struct intel_gt *gt;
	unsigned int id;

	for_each_gt(gt, i915, id)
		guc_submission_status_page_sanitization_enable(&gt->uc.guc);
}

/**
 * vf_post_migration_shutdown - Clean up the kernel structures after VF migration.
 * @i915: the i915 struct
 *
 * After this VM is migrated and assigned to a new VF, it is running on a new
 * hardware, and therefore all hardware-dependent states and related structures
 * are no longer valid.
 * By using selected parts from suspend scenario we can check whether any jobs
 * were able to finish before the migration (some might have finished at such
 * moment that the information did not made it back), and clean all the
 * invalidated structures.
 */
static void vf_post_migration_shutdown(struct drm_i915_private *i915)
{
	struct intel_gt *gt;
	unsigned int id;

	heartbeats_disable(i915);
	submissions_disable(i915);
	i915_gem_drain_freed_objects(i915);
	for_each_gt(gt, i915, id)
		intel_uc_suspend(&gt->uc);
	vf_post_migration_status_page_sanitization_disable(i915);
}

/**
 * vf_post_migration_reset_guc_state - Reset GuC state.
 * @i915: the i915 struct
 *
 * This function sends VF state reset to GuC, which also checks for the MIGRATED
 * flag, and re-schedules post-migration worker if the flag was raised.
 */
static void vf_post_migration_reset_guc_state(struct drm_i915_private *i915)
{
	intel_wakeref_t wakeref;

	with_intel_runtime_pm(&i915->runtime_pm, wakeref) {
		struct intel_gt *gt;
		unsigned int id;

		for_each_gt(gt, i915, id) {
			__intel_gt_reset(gt, ALL_ENGINES);
		}
	}
}

static bool vf_post_migration_is_scheduled(struct drm_i915_private *i915)
{
	return work_pending(&i915->sriov.vf.migration_worker);
}

static int vf_post_migration_reinit_guc(struct drm_i915_private *i915)
{
	intel_wakeref_t wakeref;
	int err;

	with_intel_runtime_pm(&i915->runtime_pm, wakeref) {
		struct intel_gt *gt;
		unsigned int id;

		for_each_gt(gt, i915, id) {
			err = intel_iov_migration_reinit_guc(&gt->iov);
			if (unlikely(err))
				break;
		}
	}
	return err;
}

static void vf_post_migration_fixup_ggtt_nodes(struct drm_i915_private *i915)
{
	struct intel_gt *gt;
	unsigned int id;

	for_each_gt(gt, i915, id) {
		/* media doesn't have its own ggtt */
		if (gt->type == GT_MEDIA)
			continue;
		intel_iov_migration_fixup_ggtt_nodes(&gt->iov);
	}
}

/**
 * vf_post_migration_kickstart - Re-initialize the driver under new hardware.
 * @i915: the i915 struct
 *
 * After we have finished with all post-migration fixes, restart the driver
 * using selected parts from resume scenario.
 */
static void vf_post_migration_kickstart(struct drm_i915_private *i915)
{
	struct intel_gt *gt;
	unsigned int id;

	for_each_gt(gt, i915, id)
		intel_uc_resume_early(&gt->uc);
	intel_runtime_pm_enable_interrupts(i915);
	i915_gem_resume(i915);
	vf_post_migration_status_page_sanitization_enable(i915);
	submissions_restore(i915);
	heartbeats_restore(i915, true);
}

static void i915_reset_backoff_enter(struct drm_i915_private *i915)
{
	struct intel_gt *gt;
	unsigned int id;

	/* Raise flag for any other resets to back off and resign. */
	for_each_gt(gt, i915, id)
		intel_gt_reset_backoff_raise(gt);

	/* Make sure intel_gt_reset_trylock() sees the I915_RESET_BACKOFF. */
	synchronize_rcu_expedited();

	/*
	 * Wait for any operations already in progress which state could be
	 * skewed by post-migration actions.
	 */
	for_each_gt(gt, i915, id)
		synchronize_srcu_expedited(&gt->reset.backoff_srcu);
}

static void i915_reset_backoff_leave(struct drm_i915_private *i915)
{
	struct intel_gt *gt;
	unsigned int id;

	for_each_gt(gt, i915, id) {
		intel_gt_reset_backoff_clear(gt);
	}
}

static void vf_post_migration_recovery(struct drm_i915_private *i915)
{
	int err;

	i915_reset_backoff_enter(i915);

	drm_dbg(&i915->drm, "migration recovery in progress\n");
	vf_post_migration_shutdown(i915);

	/*
	 * After migration has happened, all requests sent to GuC are expected
	 * to fail. Only after successful VF state reset, the VF driver can
	 * re-init GuC communication. If the VF state reset fails, it shall be
	 * repeated until success - we will skip this run and retry in that
	 * newly scheduled one.
	 */
	vf_post_migration_reset_guc_state(i915);
	if (vf_post_migration_is_scheduled(i915))
		goto defer;
	err = vf_post_migration_reinit_guc(i915);
	if (unlikely(err))
		goto fail;

	vf_post_migration_fixup_ggtt_nodes(i915);
	vf_post_migration_fixup_contexts(i915);

	vf_post_migration_kickstart(i915);
	i915_reset_backoff_leave(i915);
	drm_notice(&i915->drm, "migration recovery completed\n");
	return;

defer:
	drm_dbg(&i915->drm, "migration recovery deferred\n");
	/* We bumped wakerefs when disabling heartbeat. Put them back. */
	heartbeats_restore(i915, false);
	i915_reset_backoff_leave(i915);
	return;

fail:
	drm_err(&i915->drm, "migration recovery failed (%pe)\n", ERR_PTR(err));
	intel_gt_set_wedged(to_gt(i915));
	i915_reset_backoff_leave(i915);
}

static void migration_worker_func(struct work_struct *w)
{
	struct drm_i915_private *i915 = container_of(w, struct drm_i915_private,
						     sriov.vf.migration_worker);

	vf_post_migration_recovery(i915);
}

/**
 * i915_sriov_vf_start_migration_recovery - Start VF migration recovery.
 * @i915: the i915 struct
 *
 * This function shall be called only by VF.
 */
void i915_sriov_vf_start_migration_recovery(struct drm_i915_private *i915)
{
	bool started;

	GEM_BUG_ON(!IS_SRIOV_VF(i915));

	started = queue_work(system_unbound_wq, &i915->sriov.vf.migration_worker);
	dev_info(i915->drm.dev, "VF migration recovery %s\n", started ?
		 "scheduled" : "already in progress");
}
