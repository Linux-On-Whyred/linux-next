// SPDX-License-Identifier: GPL-2.0
/*
 * arch/sh/kernel/traps_64.c
 *
 * Copyright (C) 2000, 2001  Paolo Alberelli
 * Copyright (C) 2003, 2004  Paul Mundt
 * Copyright (C) 2003, 2004  Richard Curnow
 */
#include <linux/sched.h>
#include <linux/sched/debug.h>
#include <linux/kernel.h>
#include <linux/string.h>
#include <linux/errno.h>
#include <linux/ptrace.h>
#include <linux/timer.h>
#include <linux/mm.h>
#include <linux/smp.h>
#include <linux/init.h>
#include <linux/delay.h>
#include <linux/spinlock.h>
#include <linux/kallsyms.h>
#include <linux/interrupt.h>
#include <linux/sysctl.h>
#include <linux/module.h>
#include <linux/perf_event.h>
#include <linux/uaccess.h>
#include <asm/io.h>
#include <asm/alignment.h>
#include <asm/processor.h>
#include <asm/fpu.h>

static int read_opcode(reg_size_t pc, insn_size_t *result_opcode, int from_user_mode)
{
	int get_user_error;
	unsigned long aligned_pc;
	insn_size_t opcode;

	if ((pc & 3) == 1) {
		/* SHmedia */
		aligned_pc = pc & ~3;
		if (from_user_mode) {
			if (!access_ok(aligned_pc, sizeof(insn_size_t))) {
				get_user_error = -EFAULT;
			} else {
				get_user_error = __get_user(opcode, (insn_size_t *)aligned_pc);
				*result_opcode = opcode;
			}
			return get_user_error;
		} else {
			/* If the fault was in the kernel, we can either read
			 * this directly, or if not, we fault.
			*/
			*result_opcode = *(insn_size_t *)aligned_pc;
			return 0;
		}
	} else if ((pc & 1) == 0) {
		/* SHcompact */
		/* TODO : provide handling for this.  We don't really support
		   user-mode SHcompact yet, and for a kernel fault, this would
		   have to come from a module built for SHcompact.  */
		return -EFAULT;
	} else {
		/* misaligned */
		return -EFAULT;
	}
}

static int address_is_sign_extended(__u64 a)
{
	__u64 b;
#if (NEFF == 32)
	b = (__u64)(__s64)(__s32)(a & 0xffffffffUL);
	return (b == a) ? 1 : 0;
#else
#error "Sign extend check only works for NEFF==32"
#endif
}

/* return -1 for fault, 0 for OK */
static int generate_and_check_address(struct pt_regs *regs,
				      insn_size_t opcode,
				      int displacement_not_indexed,
				      int width_shift,
				      __u64 *address)
{
	__u64 base_address, addr;
	int basereg;

	switch (1 << width_shift) {
	case 1: inc_unaligned_byte_access(); break;
	case 2: inc_unaligned_word_access(); break;
	case 4: inc_unaligned_dword_access(); break;
	case 8: inc_unaligned_multi_access(); break;
	}

	basereg = (opcode >> 20) & 0x3f;
	base_address = regs->regs[basereg];
	if (displacement_not_indexed) {
		__s64 displacement;
		displacement = (opcode >> 10) & 0x3ff;
		displacement = sign_extend64(displacement, 9);
		addr = (__u64)((__s64)base_address + (displacement << width_shift));
	} else {
		__u64 offset;
		int offsetreg;
		offsetreg = (opcode >> 10) & 0x3f;
		offset = regs->regs[offsetreg];
		addr = base_address + offset;
	}

	/* Check sign extended */
	if (!address_is_sign_extended(addr))
		return -1;

	/* Check accessible.  For misaligned access in the kernel, assume the
	   address is always accessible (and if not, just fault when the
	   load/store gets done.) */
	if (user_mode(regs)) {
		inc_unaligned_user_access();

		if (addr >= TASK_SIZE)
			return -1;
	} else
		inc_unaligned_kernel_access();

	*address = addr;

	perf_sw_event(PERF_COUNT_SW_EMULATION_FAULTS, 1, regs, addr);
	unaligned_fixups_notify(current, opcode, regs);

	return 0;
}

static void misaligned_kernel_word_load(__u64 address, int do_sign_extend, __u64 *result)
{
	unsigned short x;
	unsigned char *p, *q;
	p = (unsigned char *) (int) address;
	q = (unsigned char *) &x;
	q[0] = p[0];
	q[1] = p[1];

	if (do_sign_extend) {
		*result = (__u64)(__s64) *(short *) &x;
	} else {
		*result = (__u64) x;
	}
}

static void misaligned_kernel_word_store(__u64 address, __u64 value)
{
	unsigned short x;
	unsigned char *p, *q;
	p = (unsigned char *) (int) address;
	q = (unsigned char *) &x;

	x = (__u16) value;
	p[0] = q[0];
	p[1] = q[1];
}

static int misaligned_load(struct pt_regs *regs,
			   insn_size_t opcode,
			   int displacement_not_indexed,
			   int width_shift,
			   int do_sign_extend)
{
	/* Return -1 for a fault, 0 for OK */
	int error;
	int destreg;
	__u64 address;

	error = generate_and_check_address(regs, opcode,
			displacement_not_indexed, width_shift, &address);
	if (error < 0)
		return error;

	destreg = (opcode >> 4) & 0x3f;
	if (user_mode(regs)) {
		__u64 buffer;

		if (!access_ok((unsigned long) address, 1UL<<width_shift)) {
			return -1;
		}

		if (__copy_user(&buffer, (const void *)(int)address, (1 << width_shift)) > 0) {
			return -1; /* fault */
		}
		switch (width_shift) {
		case 1:
			if (do_sign_extend) {
				regs->regs[destreg] = (__u64)(__s64) *(__s16 *) &buffer;
			} else {
				regs->regs[destreg] = (__u64) *(__u16 *) &buffer;
			}
			break;
		case 2:
			regs->regs[destreg] = (__u64)(__s64) *(__s32 *) &buffer;
			break;
		case 3:
			regs->regs[destreg] = buffer;
			break;
		default:
			printk("Unexpected width_shift %d in misaligned_load, PC=%08lx\n",
				width_shift, (unsigned long) regs->pc);
			break;
		}
	} else {
		/* kernel mode - we can take short cuts since if we fault, it's a genuine bug */
		__u64 lo, hi;

		switch (width_shift) {
		case 1:
			misaligned_kernel_word_load(address, do_sign_extend, &regs->regs[destreg]);
			break;
		case 2:
			asm ("ldlo.l %1, 0, %0" : "=r" (lo) : "r" (address));
			asm ("ldhi.l %1, 3, %0" : "=r" (hi) : "r" (address));
			regs->regs[destreg] = lo | hi;
			break;
		case 3:
			asm ("ldlo.q %1, 0, %0" : "=r" (lo) : "r" (address));
			asm ("ldhi.q %1, 7, %0" : "=r" (hi) : "r" (address));
			regs->regs[destreg] = lo | hi;
			break;

		default:
			printk("Unexpected width_shift %d in misaligned_load, PC=%08lx\n",
				width_shift, (unsigned long) regs->pc);
			break;
		}
	}

	return 0;
}

static int misaligned_store(struct pt_regs *regs,
			    insn_size_t opcode,
			    int displacement_not_indexed,
			    int width_shift)
{
	/* Return -1 for a fault, 0 for OK */
	int error;
	int srcreg;
	__u64 address;

	error = generate_and_check_address(regs, opcode,
			displacement_not_indexed, width_shift, &address);
	if (error < 0)
		return error;

	srcreg = (opcode >> 4) & 0x3f;
	if (user_mode(regs)) {
		__u64 buffer;

		if (!access_ok((unsigned long) address, 1UL<<width_shift)) {
			return -1;
		}

		switch (width_shift) {
		case 1:
			*(__u16 *) &buffer = (__u16) regs->regs[srcreg];
			break;
		case 2:
			*(__u32 *) &buffer = (__u32) regs->regs[srcreg];
			break;
		case 3:
			buffer = regs->regs[srcreg];
			break;
		default:
			printk("Unexpected width_shift %d in misaligned_store, PC=%08lx\n",
				width_shift, (unsigned long) regs->pc);
			break;
		}

		if (__copy_user((void *)(int)address, &buffer, (1 << width_shift)) > 0) {
			return -1; /* fault */
		}
	} else {
		/* kernel mode - we can take short cuts since if we fault, it's a genuine bug */
		__u64 val = regs->regs[srcreg];

		switch (width_shift) {
		case 1:
			misaligned_kernel_word_store(address, val);
			break;
		case 2:
			asm ("stlo.l %1, 0, %0" : : "r" (val), "r" (address));
			asm ("sthi.l %1, 3, %0" : : "r" (val), "r" (address));
			break;
		case 3:
			asm ("stlo.q %1, 0, %0" : : "r" (val), "r" (address));
			asm ("sthi.q %1, 7, %0" : : "r" (val), "r" (address));
			break;

		default:
			printk("Unexpected width_shift %d in misaligned_store, PC=%08lx\n",
				width_shift, (unsigned long) regs->pc);
			break;
		}
	}

	return 0;
}

/* Never need to fix up misaligned FPU accesses within the kernel since that's a real
   error. */
static int misaligned_fpu_load(struct pt_regs *regs,
			   insn_size_t opcode,
			   int displacement_not_indexed,
			   int width_shift,
			   int do_paired_load)
{
	/* Return -1 for a fault, 0 for OK */
	int error;
	int destreg;
	__u64 address;

	error = generate_and_check_address(regs, opcode,
			displacement_not_indexed, width_shift, &address);
	if (error < 0)
		return error;

	destreg = (opcode >> 4) & 0x3f;
	if (user_mode(regs)) {
		__u64 buffer;
		__u32 buflo, bufhi;

		if (!access_ok((unsigned long) address, 1UL<<width_shift)) {
			return -1;
		}

		if (__copy_user(&buffer, (const void *)(int)address, (1 << width_shift)) > 0) {
			return -1; /* fault */
		}
		/* 'current' may be the current owner of the FPU state, so
		   context switch the registers into memory so they can be
		   indexed by register number. */
		if (last_task_used_math == current) {
			enable_fpu();
			save_fpu(current);
			disable_fpu();
			last_task_used_math = NULL;
			regs->sr |= SR_FD;
		}

		buflo = *(__u32*) &buffer;
		bufhi = *(1 + (__u32*) &buffer);

		switch (width_shift) {
		case 2:
			current->thread.xstate->hardfpu.fp_regs[destreg] = buflo;
			break;
		case 3:
			if (do_paired_load) {
				current->thread.xstate->hardfpu.fp_regs[destreg] = buflo;
				current->thread.xstate->hardfpu.fp_regs[destreg+1] = bufhi;
			} else {
#if defined(CONFIG_CPU_LITTLE_ENDIAN)
				current->thread.xstate->hardfpu.fp_regs[destreg] = bufhi;
				current->thread.xstate->hardfpu.fp_regs[destreg+1] = buflo;
#else
				current->thread.xstate->hardfpu.fp_regs[destreg] = buflo;
				current->thread.xstate->hardfpu.fp_regs[destreg+1] = bufhi;
#endif
			}
			break;
		default:
			printk("Unexpected width_shift %d in misaligned_fpu_load, PC=%08lx\n",
				width_shift, (unsigned long) regs->pc);
			break;
		}
		return 0;
	} else {
		die ("Misaligned FPU load inside kernel", regs, 0);
		return -1;
	}
}

static int misaligned_fpu_store(struct pt_regs *regs,
			   insn_size_t opcode,
			   int displacement_not_indexed,
			   int width_shift,
			   int do_paired_load)
{
	/* Return -1 for a fault, 0 for OK */
	int error;
	int srcreg;
	__u64 address;

	error = generate_and_check_address(regs, opcode,
			displacement_not_indexed, width_shift, &address);
	if (error < 0)
		return error;

	srcreg = (opcode >> 4) & 0x3f;
	if (user_mode(regs)) {
		__u64 buffer;
		/* Initialise these to NaNs. */
		__u32 buflo=0xffffffffUL, bufhi=0xffffffffUL;

		if (!access_ok((unsigned long) address, 1UL<<width_shift)) {
			return -1;
		}

		/* 'current' may be the current owner of the FPU state, so
		   context switch the registers into memory so they can be
		   indexed by register number. */
		if (last_task_used_math == current) {
			enable_fpu();
			save_fpu(current);
			disable_fpu();
			last_task_used_math = NULL;
			regs->sr |= SR_FD;
		}

		switch (width_shift) {
		case 2:
			buflo = current->thread.xstate->hardfpu.fp_regs[srcreg];
			break;
		case 3:
			if (do_paired_load) {
				buflo = current->thread.xstate->hardfpu.fp_regs[srcreg];
				bufhi = current->thread.xstate->hardfpu.fp_regs[srcreg+1];
			} else {
#if defined(CONFIG_CPU_LITTLE_ENDIAN)
				bufhi = current->thread.xstate->hardfpu.fp_regs[srcreg];
				buflo = current->thread.xstate->hardfpu.fp_regs[srcreg+1];
#else
				buflo = current->thread.xstate->hardfpu.fp_regs[srcreg];
				bufhi = current->thread.xstate->hardfpu.fp_regs[srcreg+1];
#endif
			}
			break;
		default:
			printk("Unexpected width_shift %d in misaligned_fpu_store, PC=%08lx\n",
				width_shift, (unsigned long) regs->pc);
			break;
		}

		*(__u32*) &buffer = buflo;
		*(1 + (__u32*) &buffer) = bufhi;
		if (__copy_user((void *)(int)address, &buffer, (1 << width_shift)) > 0) {
			return -1; /* fault */
		}
		return 0;
	} else {
		die ("Misaligned FPU load inside kernel", regs, 0);
		return -1;
	}
}

static int misaligned_fixup(struct pt_regs *regs)
{
	insn_size_t opcode;
	int error;
	int major, minor;
	unsigned int user_action;

	user_action = unaligned_user_action();
	if (!(user_action & UM_FIXUP))
		return -1;

	error = read_opcode(regs->pc, &opcode, user_mode(regs));
	if (error < 0) {
		return error;
	}
	major = (opcode >> 26) & 0x3f;
	minor = (opcode >> 16) & 0xf;

	switch (major) {
		case (0x84>>2): /* LD.W */
			error = misaligned_load(regs, opcode, 1, 1, 1);
			break;
		case (0xb0>>2): /* LD.UW */
			error = misaligned_load(regs, opcode, 1, 1, 0);
			break;
		case (0x88>>2): /* LD.L */
			error = misaligned_load(regs, opcode, 1, 2, 1);
			break;
		case (0x8c>>2): /* LD.Q */
			error = misaligned_load(regs, opcode, 1, 3, 0);
			break;

		case (0xa4>>2): /* ST.W */
			error = misaligned_store(regs, opcode, 1, 1);
			break;
		case (0xa8>>2): /* ST.L */
			error = misaligned_store(regs, opcode, 1, 2);
			break;
		case (0xac>>2): /* ST.Q */
			error = misaligned_store(regs, opcode, 1, 3);
			break;

		case (0x40>>2): /* indexed loads */
			switch (minor) {
				case 0x1: /* LDX.W */
					error = misaligned_load(regs, opcode, 0, 1, 1);
					break;
				case 0x5: /* LDX.UW */
					error = misaligned_load(regs, opcode, 0, 1, 0);
					break;
				case 0x2: /* LDX.L */
					error = misaligned_load(regs, opcode, 0, 2, 1);
					break;
				case 0x3: /* LDX.Q */
					error = misaligned_load(regs, opcode, 0, 3, 0);
					break;
				default:
					error = -1;
					break;
			}
			break;

		case (0x60>>2): /* indexed stores */
			switch (minor) {
				case 0x1: /* STX.W */
					error = misaligned_store(regs, opcode, 0, 1);
					break;
				case 0x2: /* STX.L */
					error = misaligned_store(regs, opcode, 0, 2);
					break;
				case 0x3: /* STX.Q */
					error = misaligned_store(regs, opcode, 0, 3);
					break;
				default:
					error = -1;
					break;
			}
			break;

		case (0x94>>2): /* FLD.S */
			error = misaligned_fpu_load(regs, opcode, 1, 2, 0);
			break;
		case (0x98>>2): /* FLD.P */
			error = misaligned_fpu_load(regs, opcode, 1, 3, 1);
			break;
		case (0x9c>>2): /* FLD.D */
			error = misaligned_fpu_load(regs, opcode, 1, 3, 0);
			break;
		case (0x1c>>2): /* floating indexed loads */
			switch (minor) {
			case 0x8: /* FLDX.S */
				error = misaligned_fpu_load(regs, opcode, 0, 2, 0);
				break;
			case 0xd: /* FLDX.P */
				error = misaligned_fpu_load(regs, opcode, 0, 3, 1);
				break;
			case 0x9: /* FLDX.D */
				error = misaligned_fpu_load(regs, opcode, 0, 3, 0);
				break;
			default:
				error = -1;
				break;
			}
			break;
		case (0xb4>>2): /* FLD.S */
			error = misaligned_fpu_store(regs, opcode, 1, 2, 0);
			break;
		case (0xb8>>2): /* FLD.P */
			error = misaligned_fpu_store(regs, opcode, 1, 3, 1);
			break;
		case (0xbc>>2): /* FLD.D */
			error = misaligned_fpu_store(regs, opcode, 1, 3, 0);
			break;
		case (0x3c>>2): /* floating indexed stores */
			switch (minor) {
			case 0x8: /* FSTX.S */
				error = misaligned_fpu_store(regs, opcode, 0, 2, 0);
				break;
			case 0xd: /* FSTX.P */
				error = misaligned_fpu_store(regs, opcode, 0, 3, 1);
				break;
			case 0x9: /* FSTX.D */
				error = misaligned_fpu_store(regs, opcode, 0, 3, 0);
				break;
			default:
				error = -1;
				break;
			}
			break;

		default:
			/* Fault */
			error = -1;
			break;
	}

	if (error < 0) {
		return error;
	} else {
		regs->pc += 4; /* Skip the instruction that's just been emulated */
		return 0;
	}
}

static void do_unhandled_exception(int signr, char *str, unsigned long error,
				   struct pt_regs *regs)
{
	if (user_mode(regs))
		force_sig(signr);

	die_if_no_fixup(str, regs, error);
}

#define DO_ERROR(signr, str, name) \
asmlinkage void do_##name(unsigned long error_code, struct pt_regs *regs) \
{ \
	do_unhandled_exception(signr, str, error_code, regs); \
}

DO_ERROR(SIGILL,  "illegal slot instruction", illegal_slot_inst)
DO_ERROR(SIGSEGV, "address error (exec)", address_error_exec)

#if defined(CONFIG_SH64_ID2815_WORKAROUND)

#define OPCODE_INVALID      0
#define OPCODE_USER_VALID   1
#define OPCODE_PRIV_VALID   2

/* getcon/putcon - requires checking which control register is referenced. */
#define OPCODE_CTRL_REG     3

/* Table of valid opcodes for SHmedia mode.
   Form a 10-bit value by concatenating the major/minor opcodes i.e.
   opcode[31:26,20:16].  The 6 MSBs of this value index into the following
   array.  The 4 LSBs select the bit-pair in the entry (bits 1:0 correspond to
   LSBs==4'b0000 etc). */
static unsigned long shmedia_opcode_table[64] = {
	0x55554044,0x54445055,0x15141514,0x14541414,0x00000000,0x10001000,0x01110055,0x04050015,
	0x00000444,0xc0000000,0x44545515,0x40405555,0x55550015,0x10005555,0x55555505,0x04050000,
	0x00000555,0x00000404,0x00040445,0x15151414,0x00000000,0x00000000,0x00000000,0x00000000,
	0x00000055,0x40404444,0x00000404,0xc0009495,0x00000000,0x00000000,0x00000000,0x00000000,
	0x55555555,0x55555555,0x55555555,0x55555555,0x55555555,0x55555555,0x55555555,0x55555555,
	0x55555555,0x55555555,0x55555555,0x55555555,0x55555555,0x55555555,0x55555555,0x55555555,
	0x80005050,0x04005055,0x55555555,0x55555555,0x55555555,0x55555555,0x55555555,0x55555555,
	0x81055554,0x00000404,0x55555555,0x55555555,0x00000000,0x00000000,0x00000000,0x00000000
};

/* Workaround SH5-101 cut2 silicon defect #2815 :
   in some situations, inter-mode branches from SHcompact -> SHmedia
   which should take ITLBMISS or EXECPROT exceptions at the target
   falsely take RESINST at the target instead. */
void do_reserved_inst(unsigned long error_code, struct pt_regs *regs)
{
	insn_size_t opcode = 0x6ff4fff0; /* guaranteed reserved opcode */
	unsigned long pc, aligned_pc;
	unsigned long index, shift;
	unsigned long major, minor, combined;
	unsigned long reserved_field;
	int opcode_state;
	int get_user_error;
	int signr = SIGILL;
	char *exception_name = "reserved_instruction";

	pc = regs->pc;

	/* SHcompact is not handled */
	if (unlikely((pc & 3) == 0))
		goto out;

	/* SHmedia : check for defect.  This requires executable vmas
	   to be readable too. */
	aligned_pc = pc & ~3;
	if (!access_ok(aligned_pc, sizeof(insn_size_t)))
		get_user_error = -EFAULT;
	else
		get_user_error = __get_user(opcode, (insn_size_t *)aligned_pc);

	if (get_user_error < 0) {
		/*
		 * Error trying to read opcode.  This typically means a
		 * real fault, not a RESINST any more.  So change the
		 * codes.
		 */
		exception_name = "address error (exec)";
		signr = SIGSEGV;
		goto out;
	}

	/* These bits are currently reserved as zero in all valid opcodes */
	reserved_field = opcode & 0xf;
	if (unlikely(reserved_field))
		goto out;	/* invalid opcode */

	major = (opcode >> 26) & 0x3f;
	minor = (opcode >> 16) & 0xf;
	combined = (major << 4) | minor;
	index = major;
	shift = minor << 1;
	opcode_state = (shmedia_opcode_table[index] >> shift) & 0x3;
	switch (opcode_state) {
	case OPCODE_INVALID:
		/* Trap. */
		break;
	case OPCODE_USER_VALID:
		/*
		 * Restart the instruction: the branch to the instruction
		 * will now be from an RTE not from SHcompact so the
		 * silicon defect won't be triggered.
		 */
		return;
	case OPCODE_PRIV_VALID:
		if (!user_mode(regs)) {
			/*
			 * Should only ever get here if a module has
			 * SHcompact code inside it. If so, the same fix
			 * up is needed.
			 */
			return; /* same reason */
		}

		/*
		 * Otherwise, user mode trying to execute a privileged
		 * instruction - fall through to trap.
		 */
		break;
	case OPCODE_CTRL_REG:
		/* If in privileged mode, return as above. */
		if (!user_mode(regs))
			return;

		/* In user mode ... */
		if (combined == 0x9f) { /* GETCON */
			unsigned long regno = (opcode >> 20) & 0x3f;

			if (regno >= 62)
				return;

			/* reserved/privileged control register => trap */
		} else if (combined == 0x1bf) { /* PUTCON */
			unsigned long regno = (opcode >> 4) & 0x3f;

			if (regno >= 62)
				return;

			/* reserved/privileged control register => trap */
		}

		break;
	default:
		/* Fall through to trap. */
		break;
	}

out:
	do_unhandled_exception(signr, exception_name, error_code, regs);
}

#else /* CONFIG_SH64_ID2815_WORKAROUND */

/* If the workaround isn't needed, this is just a straightforward reserved
   instruction */
DO_ERROR(SIGILL, "reserved instruction", reserved_inst)

#endif /* CONFIG_SH64_ID2815_WORKAROUND */

/* Called with interrupts disabled */
asmlinkage void do_exception_error(unsigned long ex, struct pt_regs *regs)
{
	die_if_kernel("exception", regs, ex);
}

asmlinkage int do_unknown_trapa(unsigned long scId, struct pt_regs *regs)
{
	/* Syscall debug */
	printk("System call ID error: [0x1#args:8 #syscall:16  0x%lx]\n", scId);

	die_if_kernel("unknown trapa", regs, scId);

	return -ENOSYS;
}

/* Implement misaligned load/store handling for kernel (and optionally for user
   mode too).  Limitation : only SHmedia mode code is handled - there is no
   handling at all for misaligned accesses occurring in SHcompact code yet. */

asmlinkage void do_address_error_load(unsigned long error_code, struct pt_regs *regs)
{
	if (misaligned_fixup(regs) < 0)
		do_unhandled_exception(SIGSEGV, "address error(load)",
				       error_code, regs);
}

asmlinkage void do_address_error_store(unsigned long error_code, struct pt_regs *regs)
{
	if (misaligned_fixup(regs) < 0)
		do_unhandled_exception(SIGSEGV, "address error(store)",
				error_code, regs);
}

asmlinkage void do_debug_interrupt(unsigned long code, struct pt_regs *regs)
{
	u64 peek_real_address_q(u64 addr);
	u64 poke_real_address_q(u64 addr, u64 val);
	unsigned long long DM_EXP_CAUSE_PHY = 0x0c100010;
	unsigned long long exp_cause;
	/* It's not worth ioremapping the debug module registers for the amount
	   of access we make to them - just go direct to their physical
	   addresses. */
	exp_cause = peek_real_address_q(DM_EXP_CAUSE_PHY);
	if (exp_cause & ~4)
		printk("DM.EXP_CAUSE had unexpected bits set (=%08lx)\n",
			(unsigned long)(exp_cause & 0xffffffff));
	show_state();
	/* Clear all DEBUGINT causes */
	poke_real_address_q(DM_EXP_CAUSE_PHY, 0x0);
}

void per_cpu_trap_init(void)
{
	/* Nothing to do for now, VBR initialization later. */
}
