#
# SPDX-License-Identifier: BSD-2-Clause
#
# Copyright (c) 2019 Western Digital Corporation or its affiliates.
#
# Authors:
#   Anup Patel <anup.patel@wdc.com>
#

libsbi-objs-y += riscv_asm.o
libsbi-objs-y += riscv_atomic.o
libsbi-objs-y += riscv_hardfp.o
libsbi-objs-y += riscv_locks.o

libsbi-objs-y += sbi_console.o
libsbi-objs-y += sbi_ecall.o
libsbi-objs-y += sbi_ecall_base.o
libsbi-objs-y += sbi_ecall_legacy.o
libsbi-objs-y += sbi_ecall_replace.o
libsbi-objs-y += sbi_ecall_vendor.o
libsbi-objs-y += sbi_emulate_csr.o
libsbi-objs-y += sbi_fifo.o
libsbi-objs-y += sbi_hfence.o
libsbi-objs-y += sbi_hart.o
libsbi-objs-y += sbi_illegal_insn.o
libsbi-objs-y += sbi_init.o
libsbi-objs-y += sbi_ipi.o
libsbi-objs-y += sbi_misaligned_ldst.o
libsbi-objs-y += sbi_scratch.o
libsbi-objs-y += sbi_system.o
libsbi-objs-y += sbi_timer.o
libsbi-objs-y += sbi_tlb.o
libsbi-objs-y += sbi_trap.o
libsbi-objs-y += sbi_string.o
libsbi-objs-y += sbi_unpriv.o

## Add by Dong Du
#  The Penglai related files here
libsbi-objs-y += sm/enclave.o
libsbi-objs-y += sm/ipi.o
libsbi-objs-y += sm/math.o
libsbi-objs-y += sm/pmp.o
libsbi-objs-y += sm/sm.o
libsbi-objs-y += sm/thread.o
#libsbi-objs-y += sm/platform/spmp/enclave_mm.o
#libsbi-objs-y += sm/platform/spmp/ipi_handler.o
libsbi-objs-y += sm/platform/spmp/platform.o
#libsbi-objs-y += sm/platform/spmp/platform_thread.o
#libsbi-objs-y += sm/platform/spmp/spmp.o
