# SPDX-License-Identifier: GPL-2.0
#
# Makefile for drivers/platform/x86/intel
# Intel x86 Platform-Specific Drivers
#
#
#obj-$(CONFIG_INTEL_ATOMISP2_PDX86)	+= atomisp2/
#obj-$(CONFIG_INTEL_SAR_INT1092)		+= int1092/
#obj-$(CONFIG_INTEL_CHT_INT33FE)		+= int33fe/
#obj-$(CONFIG_INTEL_SKL_INT3472)		+= int3472/
#obj-$(CONFIG_INTEL_PMC_CORE)		+= pmc/
obj-$(CPTCFG_INTEL_PMT_CLASS)		+= pmt/
#obj-$(CONFIG_INTEL_SPEED_SELECT_INTERFACE) += speed_select_if/
#obj-$(CONFIG_INTEL_TELEMETRY)		+= telemetry/
#obj-$(CONFIG_INTEL_WMI)			+= wmi/

# Intel input drivers
intel-hid-y				:= hid.o
obj-$(CPTCFG_INTEL_HID_EVENT)		+= intel-hid.o
intel-vbtn-y				:= vbtn.o
obj-$(CPTCFG_INTEL_VBTN)		+= intel-vbtn.o

# Intel miscellaneous drivers
intel_int0002_vgpio-y			:= int0002_vgpio.o
obj-$(CPTCFG_INTEL_INT0002_VGPIO)	+= intel_int0002_vgpio.o
intel_oaktrail-y			:= oaktrail.o
obj-$(CPTCFG_INTEL_OAKTRAIL)		+= intel_oaktrail.o
intel_vsec-y				:= vsec.o
obj-$(CPTCFG_INTEL_VSEC)		+= intel_vsec.o

# Intel PMIC / PMC / P-Unit drivers
intel_bxtwc_tmu-y			:= bxtwc_tmu.o
obj-$(CPTCFG_INTEL_BXTWC_PMIC_TMU)	+= intel_bxtwc_tmu.o
intel_chtdc_ti_pwrbtn-y			:= chtdc_ti_pwrbtn.o
obj-$(CPTCFG_INTEL_CHTDC_TI_PWRBTN)	+= intel_chtdc_ti_pwrbtn.o
intel_mrfld_pwrbtn-y			:= mrfld_pwrbtn.o
obj-$(CPTCFG_INTEL_MRFLD_PWRBTN)	+= intel_mrfld_pwrbtn.o
intel_punit_ipc-y			:= punit_ipc.o
obj-$(CPTCFG_INTEL_PUNIT_IPC)		+= intel_punit_ipc.o

# Intel Uncore drivers
intel-rst-y				:= rst.o
obj-$(CPTCFG_INTEL_RST)			+= intel-rst.o
intel-smartconnect-y			:= smartconnect.o
obj-$(CPTCFG_INTEL_SMARTCONNECT)	+= intel-smartconnect.o
intel_turbo_max_3-y			:= turbo_max_3.o
obj-$(CPTCFG_INTEL_TURBO_MAX_3)		+= intel_turbo_max_3.o
intel-uncore-frequency-y		:= uncore-frequency.o
obj-$(CPTCFG_INTEL_UNCORE_FREQ_CONTROL)	+= intel-uncore-frequency.o
