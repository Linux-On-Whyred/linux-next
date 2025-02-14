// SPDX-License-Identifier: GPL-2.0-only
/*
 * Kernel-based Virtual Machine driver for Linux
 *
 * AMD SVM support
 *
 * Copyright (C) 2006 Qumranet, Inc.
 * Copyright 2010 Red Hat, Inc. and/or its affiliates.
 *
 * Authors:
 *   Yaniv Kamay  <yaniv@qumranet.com>
 *   Avi Kivity   <avi@qumranet.com>
 */

#ifndef __SVM_SVM_H
#define __SVM_SVM_H

#include <linux/kvm_types.h>
#include <linux/kvm_host.h>

#include <asm/svm.h>

static const u32 host_save_user_msrs[] = {
#ifdef CONFIG_X86_64
	MSR_STAR, MSR_LSTAR, MSR_CSTAR, MSR_SYSCALL_MASK, MSR_KERNEL_GS_BASE,
	MSR_FS_BASE,
#endif
	MSR_IA32_SYSENTER_CS, MSR_IA32_SYSENTER_ESP, MSR_IA32_SYSENTER_EIP,
	MSR_TSC_AUX,
};

#define NR_HOST_SAVE_USER_MSRS ARRAY_SIZE(host_save_user_msrs)

#define MSRPM_OFFSETS	16
extern u32 msrpm_offsets[MSRPM_OFFSETS] __read_mostly;
extern bool npt_enabled;

enum {
	VMCB_INTERCEPTS, /* Intercept vectors, TSC offset,
			    pause filter count */
	VMCB_PERM_MAP,   /* IOPM Base and MSRPM Base */
	VMCB_ASID,	 /* ASID */
	VMCB_INTR,	 /* int_ctl, int_vector */
	VMCB_NPT,        /* npt_en, nCR3, gPAT */
	VMCB_CR,	 /* CR0, CR3, CR4, EFER */
	VMCB_DR,         /* DR6, DR7 */
	VMCB_DT,         /* GDT, IDT */
	VMCB_SEG,        /* CS, DS, SS, ES, CPL */
	VMCB_CR2,        /* CR2 only */
	VMCB_LBR,        /* DBGCTL, BR_FROM, BR_TO, LAST_EX_FROM, LAST_EX_TO */
	VMCB_AVIC,       /* AVIC APIC_BAR, AVIC APIC_BACKING_PAGE,
			  * AVIC PHYSICAL_TABLE pointer,
			  * AVIC LOGICAL_TABLE pointer
			  */
	VMCB_DIRTY_MAX,
};

/* TPR and CR2 are always written before VMRUN */
#define VMCB_ALWAYS_DIRTY_MASK	((1U << VMCB_INTR) | (1U << VMCB_CR2))

struct kvm_sev_info {
	bool active;		/* SEV enabled guest */
	unsigned int asid;	/* ASID used for this guest */
	unsigned int handle;	/* SEV firmware handle */
	int fd;			/* SEV device fd */
	unsigned long pages_locked; /* Number of pages locked */
	struct list_head regions_list;  /* List of registered regions */
};

struct kvm_svm {
	struct kvm kvm;

	/* Struct members for AVIC */
	u32 avic_vm_id;
	struct page *avic_logical_id_table_page;
	struct page *avic_physical_id_table_page;
	struct hlist_node hnode;

	struct kvm_sev_info sev_info;
};

struct kvm_vcpu;

struct nested_state {
	struct vmcb *hsave;
	u64 hsave_msr;
	u64 vm_cr_msr;
	u64 vmcb;

	/* These are the merged vectors */
	u32 *msrpm;

	/* gpa pointers to the real vectors */
	u64 vmcb_msrpm;
	u64 vmcb_iopm;

	/* A VMEXIT is required but not yet emulated */
	bool exit_required;

	/* A VMRUN has started but has not yet been performed, so
	 * we cannot inject a nested vmexit yet.  */
	bool nested_run_pending;

	/* cache for intercepts of the guest */
	u32 intercept_cr;
	u32 intercept_dr;
	u32 intercept_exceptions;
	u64 intercept;

	/* Nested Paging related state */
	u64 nested_cr3;
};

struct vcpu_svm {
	struct kvm_vcpu vcpu;
	struct vmcb *vmcb;
	unsigned long vmcb_pa;
	struct svm_cpu_data *svm_data;
	uint64_t asid_generation;
	uint64_t sysenter_esp;
	uint64_t sysenter_eip;
	uint64_t tsc_aux;

	u64 msr_decfg;

	u64 next_rip;

	u64 host_user_msrs[NR_HOST_SAVE_USER_MSRS];
	struct {
		u16 fs;
		u16 gs;
		u16 ldt;
		u64 gs_base;
	} host;

	u64 spec_ctrl;
	/*
	 * Contains guest-controlled bits of VIRT_SPEC_CTRL, which will be
	 * translated into the appropriate L2_CFG bits on the host to
	 * perform speculative control.
	 */
	u64 virt_spec_ctrl;

	u32 *msrpm;

	ulong nmi_iret_rip;

	struct nested_state nested;

	bool nmi_singlestep;
	u64 nmi_singlestep_guest_rflags;

	unsigned int3_injected;
	unsigned long int3_rip;

	/* cached guest cpuid flags for faster access */
	bool nrips_enabled	: 1;

	u32 ldr_reg;
	u32 dfr_reg;
	struct page *avic_backing_page;
	u64 *avic_physical_id_cache;
	bool avic_is_running;

	/*
	 * Per-vcpu list of struct amd_svm_iommu_ir:
	 * This is used mainly to store interrupt remapping information used
	 * when update the vcpu affinity. This avoids the need to scan for
	 * IRTE and try to match ga_tag in the IOMMU driver.
	 */
	struct list_head ir_list;
	spinlock_t ir_list_lock;

	/* which host CPU was used for running this vcpu */
	unsigned int last_cpu;
};

struct svm_cpu_data {
	int cpu;

	u64 asid_generation;
	u32 max_asid;
	u32 next_asid;
	u32 min_asid;
	struct kvm_ldttss_desc *tss_desc;

	struct page *save_area;
	struct vmcb *current_vmcb;

	/* index = sev_asid, value = vmcb pointer */
	struct vmcb **sev_vmcbs;
};

DECLARE_PER_CPU(struct svm_cpu_data *, svm_data);

void recalc_intercepts(struct vcpu_svm *svm);

static inline struct kvm_svm *to_kvm_svm(struct kvm *kvm)
{
	return container_of(kvm, struct kvm_svm, kvm);
}

static inline void mark_all_dirty(struct vmcb *vmcb)
{
	vmcb->control.clean = 0;
}

static inline void mark_all_clean(struct vmcb *vmcb)
{
	vmcb->control.clean = ((1 << VMCB_DIRTY_MAX) - 1)
			       & ~VMCB_ALWAYS_DIRTY_MASK;
}

static inline void mark_dirty(struct vmcb *vmcb, int bit)
{
	vmcb->control.clean &= ~(1 << bit);
}

static inline struct vcpu_svm *to_svm(struct kvm_vcpu *vcpu)
{
	return container_of(vcpu, struct vcpu_svm, vcpu);
}

static inline struct vmcb *get_host_vmcb(struct vcpu_svm *svm)
{
	if (is_guest_mode(&svm->vcpu))
		return svm->nested.hsave;
	else
		return svm->vmcb;
}

static inline void set_cr_intercept(struct vcpu_svm *svm, int bit)
{
	struct vmcb *vmcb = get_host_vmcb(svm);

	vmcb->control.intercept_cr |= (1U << bit);

	recalc_intercepts(svm);
}

static inline void clr_cr_intercept(struct vcpu_svm *svm, int bit)
{
	struct vmcb *vmcb = get_host_vmcb(svm);

	vmcb->control.intercept_cr &= ~(1U << bit);

	recalc_intercepts(svm);
}

static inline bool is_cr_intercept(struct vcpu_svm *svm, int bit)
{
	struct vmcb *vmcb = get_host_vmcb(svm);

	return vmcb->control.intercept_cr & (1U << bit);
}

static inline void set_dr_intercepts(struct vcpu_svm *svm)
{
	struct vmcb *vmcb = get_host_vmcb(svm);

	vmcb->control.intercept_dr = (1 << INTERCEPT_DR0_READ)
		| (1 << INTERCEPT_DR1_READ)
		| (1 << INTERCEPT_DR2_READ)
		| (1 << INTERCEPT_DR3_READ)
		| (1 << INTERCEPT_DR4_READ)
		| (1 << INTERCEPT_DR5_READ)
		| (1 << INTERCEPT_DR6_READ)
		| (1 << INTERCEPT_DR7_READ)
		| (1 << INTERCEPT_DR0_WRITE)
		| (1 << INTERCEPT_DR1_WRITE)
		| (1 << INTERCEPT_DR2_WRITE)
		| (1 << INTERCEPT_DR3_WRITE)
		| (1 << INTERCEPT_DR4_WRITE)
		| (1 << INTERCEPT_DR5_WRITE)
		| (1 << INTERCEPT_DR6_WRITE)
		| (1 << INTERCEPT_DR7_WRITE);

	recalc_intercepts(svm);
}

static inline void clr_dr_intercepts(struct vcpu_svm *svm)
{
	struct vmcb *vmcb = get_host_vmcb(svm);

	vmcb->control.intercept_dr = 0;

	recalc_intercepts(svm);
}

static inline void set_exception_intercept(struct vcpu_svm *svm, int bit)
{
	struct vmcb *vmcb = get_host_vmcb(svm);

	vmcb->control.intercept_exceptions |= (1U << bit);

	recalc_intercepts(svm);
}

static inline void clr_exception_intercept(struct vcpu_svm *svm, int bit)
{
	struct vmcb *vmcb = get_host_vmcb(svm);

	vmcb->control.intercept_exceptions &= ~(1U << bit);

	recalc_intercepts(svm);
}

static inline void set_intercept(struct vcpu_svm *svm, int bit)
{
	struct vmcb *vmcb = get_host_vmcb(svm);

	vmcb->control.intercept |= (1ULL << bit);

	recalc_intercepts(svm);
}

static inline void clr_intercept(struct vcpu_svm *svm, int bit)
{
	struct vmcb *vmcb = get_host_vmcb(svm);

	vmcb->control.intercept &= ~(1ULL << bit);

	recalc_intercepts(svm);
}

static inline bool is_intercept(struct vcpu_svm *svm, int bit)
{
	return (svm->vmcb->control.intercept & (1ULL << bit)) != 0;
}

static inline bool vgif_enabled(struct vcpu_svm *svm)
{
	return !!(svm->vmcb->control.int_ctl & V_GIF_ENABLE_MASK);
}

static inline void enable_gif(struct vcpu_svm *svm)
{
	if (vgif_enabled(svm))
		svm->vmcb->control.int_ctl |= V_GIF_MASK;
	else
		svm->vcpu.arch.hflags |= HF_GIF_MASK;
}

static inline void disable_gif(struct vcpu_svm *svm)
{
	if (vgif_enabled(svm))
		svm->vmcb->control.int_ctl &= ~V_GIF_MASK;
	else
		svm->vcpu.arch.hflags &= ~HF_GIF_MASK;
}

static inline bool gif_set(struct vcpu_svm *svm)
{
	if (vgif_enabled(svm))
		return !!(svm->vmcb->control.int_ctl & V_GIF_MASK);
	else
		return !!(svm->vcpu.arch.hflags & HF_GIF_MASK);
}

/* svm.c */
#define MSR_INVALID			0xffffffffU

u32 svm_msrpm_offset(u32 msr);
void svm_set_efer(struct kvm_vcpu *vcpu, u64 efer);
void svm_set_cr0(struct kvm_vcpu *vcpu, unsigned long cr0);
int svm_set_cr4(struct kvm_vcpu *vcpu, unsigned long cr4);
void svm_flush_tlb(struct kvm_vcpu *vcpu);
void disable_nmi_singlestep(struct vcpu_svm *svm);
bool svm_smi_blocked(struct kvm_vcpu *vcpu);
bool svm_nmi_blocked(struct kvm_vcpu *vcpu);
bool svm_interrupt_blocked(struct kvm_vcpu *vcpu);

/* nested.c */

#define NESTED_EXIT_HOST	0	/* Exit handled on host level */
#define NESTED_EXIT_DONE	1	/* Exit caused nested vmexit  */
#define NESTED_EXIT_CONTINUE	2	/* Further checks needed      */

static inline bool svm_nested_virtualize_tpr(struct kvm_vcpu *vcpu)
{
	return is_guest_mode(vcpu) && (vcpu->arch.hflags & HF_VINTR_MASK);
}

static inline bool nested_exit_on_smi(struct vcpu_svm *svm)
{
	return (svm->nested.intercept & (1ULL << INTERCEPT_SMI));
}

static inline bool nested_exit_on_intr(struct vcpu_svm *svm)
{
	return (svm->nested.intercept & (1ULL << INTERCEPT_INTR));
}

static inline bool nested_exit_on_nmi(struct vcpu_svm *svm)
{
	return (svm->nested.intercept & (1ULL << INTERCEPT_NMI));
}

void enter_svm_guest_mode(struct vcpu_svm *svm, u64 vmcb_gpa,
			  struct vmcb *nested_vmcb, struct kvm_host_map *map);
int nested_svm_vmrun(struct vcpu_svm *svm);
void nested_svm_vmloadsave(struct vmcb *from_vmcb, struct vmcb *to_vmcb);
int nested_svm_vmexit(struct vcpu_svm *svm);
int nested_svm_exit_handled(struct vcpu_svm *svm);
int nested_svm_check_permissions(struct vcpu_svm *svm);
int nested_svm_check_exception(struct vcpu_svm *svm, unsigned nr,
			       bool has_error_code, u32 error_code);
int nested_svm_exit_special(struct vcpu_svm *svm);

extern struct kvm_x86_nested_ops svm_nested_ops;

/* avic.c */

#define AVIC_LOGICAL_ID_ENTRY_GUEST_PHYSICAL_ID_MASK	(0xFF)
#define AVIC_LOGICAL_ID_ENTRY_VALID_BIT			31
#define AVIC_LOGICAL_ID_ENTRY_VALID_MASK		(1 << 31)

#define AVIC_PHYSICAL_ID_ENTRY_HOST_PHYSICAL_ID_MASK	(0xFFULL)
#define AVIC_PHYSICAL_ID_ENTRY_BACKING_PAGE_MASK	(0xFFFFFFFFFFULL << 12)
#define AVIC_PHYSICAL_ID_ENTRY_IS_RUNNING_MASK		(1ULL << 62)
#define AVIC_PHYSICAL_ID_ENTRY_VALID_MASK		(1ULL << 63)

#define VMCB_AVIC_APIC_BAR_MASK		0xFFFFFFFFFF000ULL

extern int avic;

static inline void avic_update_vapic_bar(struct vcpu_svm *svm, u64 data)
{
	svm->vmcb->control.avic_vapic_bar = data & VMCB_AVIC_APIC_BAR_MASK;
	mark_dirty(svm->vmcb, VMCB_AVIC);
}

static inline bool avic_vcpu_is_running(struct kvm_vcpu *vcpu)
{
	struct vcpu_svm *svm = to_svm(vcpu);
	u64 *entry = svm->avic_physical_id_cache;

	if (!entry)
		return false;

	return (READ_ONCE(*entry) & AVIC_PHYSICAL_ID_ENTRY_IS_RUNNING_MASK);
}

int avic_ga_log_notifier(u32 ga_tag);
void avic_vm_destroy(struct kvm *kvm);
int avic_vm_init(struct kvm *kvm);
void avic_init_vmcb(struct vcpu_svm *svm);
void svm_toggle_avic_for_irq_window(struct kvm_vcpu *vcpu, bool activate);
int avic_incomplete_ipi_interception(struct vcpu_svm *svm);
int avic_unaccelerated_access_interception(struct vcpu_svm *svm);
int avic_init_vcpu(struct vcpu_svm *svm);
void avic_vcpu_load(struct kvm_vcpu *vcpu, int cpu);
void avic_vcpu_put(struct kvm_vcpu *vcpu);
void avic_post_state_restore(struct kvm_vcpu *vcpu);
void svm_set_virtual_apic_mode(struct kvm_vcpu *vcpu);
void svm_refresh_apicv_exec_ctrl(struct kvm_vcpu *vcpu);
bool svm_check_apicv_inhibit_reasons(ulong bit);
void svm_pre_update_apicv_exec_ctrl(struct kvm *kvm, bool activate);
void svm_load_eoi_exitmap(struct kvm_vcpu *vcpu, u64 *eoi_exit_bitmap);
void svm_hwapic_irr_update(struct kvm_vcpu *vcpu, int max_irr);
void svm_hwapic_isr_update(struct kvm_vcpu *vcpu, int max_isr);
int svm_deliver_avic_intr(struct kvm_vcpu *vcpu, int vec);
bool svm_dy_apicv_has_pending_interrupt(struct kvm_vcpu *vcpu);
int svm_update_pi_irte(struct kvm *kvm, unsigned int host_irq,
		       uint32_t guest_irq, bool set);
void svm_vcpu_blocking(struct kvm_vcpu *vcpu);
void svm_vcpu_unblocking(struct kvm_vcpu *vcpu);

/* sev.c */

extern unsigned int max_sev_asid;

static inline bool sev_guest(struct kvm *kvm)
{
#ifdef CONFIG_KVM_AMD_SEV
	struct kvm_sev_info *sev = &to_kvm_svm(kvm)->sev_info;

	return sev->active;
#else
	return false;
#endif
}

static inline bool svm_sev_enabled(void)
{
	return IS_ENABLED(CONFIG_KVM_AMD_SEV) ? max_sev_asid : 0;
}

void sev_vm_destroy(struct kvm *kvm);
int svm_mem_enc_op(struct kvm *kvm, void __user *argp);
int svm_register_enc_region(struct kvm *kvm,
			    struct kvm_enc_region *range);
int svm_unregister_enc_region(struct kvm *kvm,
			      struct kvm_enc_region *range);
void pre_sev_run(struct vcpu_svm *svm, int cpu);
int __init sev_hardware_setup(void);
void sev_hardware_teardown(void);

#endif
