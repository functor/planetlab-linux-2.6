/******************************************************************************
 * xen.h
 * 
 * Guest OS interface to Xen.
 * 
 * Copyright (c) 2004, K A Fraser
 */

#ifndef __XEN_PUBLIC_XEN_H__
#define __XEN_PUBLIC_XEN_H__

#if defined(__i386__)
#include "arch-x86_32.h"
#elif defined(__x86_64__)
#include "arch-x86_64.h"
#elif defined(__ia64__)
#include "arch-ia64.h"
#else
#error "Unsupported architecture"
#endif

/*
 * XEN "SYSTEM CALLS" (a.k.a. HYPERCALLS).
 */

/*
 * x86_32: EAX = vector; EBX, ECX, EDX, ESI, EDI = args 1, 2, 3, 4, 5.
 *         EAX = return value
 *         (argument registers may be clobbered on return)
 * x86_64: RAX = vector; RDI, RSI, RDX, R10, R8, R9 = args 1, 2, 3, 4, 5, 6. 
 *         RAX = return value
 *         (argument registers not clobbered on return; RCX, R11 are)
 */
#define __HYPERVISOR_set_trap_table        0
#define __HYPERVISOR_mmu_update            1
#define __HYPERVISOR_set_gdt               2
#define __HYPERVISOR_stack_switch          3
#define __HYPERVISOR_set_callbacks         4
#define __HYPERVISOR_fpu_taskswitch        5
#define __HYPERVISOR_sched_op              6
#define __HYPERVISOR_dom0_op               7
#define __HYPERVISOR_set_debugreg          8
#define __HYPERVISOR_get_debugreg          9
#define __HYPERVISOR_update_descriptor    10
#define __HYPERVISOR_set_fast_trap        11 /* x86/32 only */
#define __HYPERVISOR_dom_mem_op           12
#define __HYPERVISOR_multicall            13
#define __HYPERVISOR_update_va_mapping    14
#define __HYPERVISOR_set_timer_op         15
#define __HYPERVISOR_event_channel_op     16
#define __HYPERVISOR_xen_version          17
#define __HYPERVISOR_console_io           18
#define __HYPERVISOR_physdev_op           19
#define __HYPERVISOR_grant_table_op       20
#define __HYPERVISOR_vm_assist            21
#define __HYPERVISOR_update_va_mapping_otherdomain 22
#define __HYPERVISOR_switch_vm86          23 /* x86/32 only */
#define __HYPERVISOR_switch_to_user       23 /* x86/64 only */
#define __HYPERVISOR_boot_vcpu            24
#define __HYPERVISOR_set_segment_base     25 /* x86/64 only */
#define __HYPERVISOR_mmuext_op            26

/*
 * MULTICALLS
 * 
 * Multicalls are listed in an array, with each element being a fixed size 
 * (BYTES_PER_MULTICALL_ENTRY). Each is of the form (op, arg1, ..., argN)
 * where each element of the tuple is a machine word. 
 */
#define ARGS_PER_MULTICALL_ENTRY 8


/* 
 * VIRTUAL INTERRUPTS
 * 
 * Virtual interrupts that a guest OS may receive from Xen.
 */
#define VIRQ_TIMER      0  /* Timebase update, and/or requested timeout.  */
#define VIRQ_DEBUG      1  /* Request guest to dump debug info.           */
#define VIRQ_CONSOLE    2  /* (DOM0) bytes received on emergency console. */
#define VIRQ_DOM_EXC    3  /* (DOM0) Exceptional event for some domain.   */
#define VIRQ_PARITY_ERR 4  /* (DOM0) NMI parity error.                    */
#define VIRQ_IO_ERR     5  /* (DOM0) NMI I/O error.                       */
#define NR_VIRQS        7

/*
 * MMU-UPDATE REQUESTS
 * 
 * HYPERVISOR_mmu_update() accepts a list of (ptr, val) pairs.
 * A foreigndom (FD) can be specified (or DOMID_SELF for none).
 * Where the FD has some effect, it is described below.
 * ptr[1:0] specifies the appropriate MMU_* command.
 * 
 * ptr[1:0] == MMU_NORMAL_PT_UPDATE:
 * Updates an entry in a page table. If updating an L1 table, and the new
 * table entry is valid/present, the mapped frame must belong to the FD, if
 * an FD has been specified. If attempting to map an I/O page then the
 * caller assumes the privilege of the FD.
 * FD == DOMID_IO: Permit /only/ I/O mappings, at the priv level of the caller.
 * FD == DOMID_XEN: Map restricted areas of Xen's heap space.
 * ptr[:2]  -- Machine address of the page-table entry to modify.
 * val      -- Value to write.
 * 
 * ptr[1:0] == MMU_MACHPHYS_UPDATE:
 * Updates an entry in the machine->pseudo-physical mapping table.
 * ptr[:2]  -- Machine address within the frame whose mapping to modify.
 *             The frame must belong to the FD, if one is specified.
 * val      -- Value to write into the mapping entry.
 */
#define MMU_NORMAL_PT_UPDATE     0 /* checked '*ptr = val'. ptr is MA.       */
#define MMU_MACHPHYS_UPDATE      1 /* ptr = MA of frame to modify entry for  */

/*
 * MMU EXTENDED OPERATIONS
 * 
 * HYPERVISOR_mmuext_op() accepts a list of mmuext_op structures.
 * A foreigndom (FD) can be specified (or DOMID_SELF for none).
 * Where the FD has some effect, it is described below.
 * 
 * cmd: MMUEXT_(UN)PIN_*_TABLE
 * mfn: Machine frame number to be (un)pinned as a p.t. page.
 *      The frame must belong to the FD, if one is specified.
 * 
 * cmd: MMUEXT_NEW_BASEPTR
 * mfn: Machine frame number of new page-table base to install in MMU.
 * 
 * cmd: MMUEXT_NEW_USER_BASEPTR [x86/64 only]
 * mfn: Machine frame number of new page-table base to install in MMU
 *      when in user space.
 * 
 * cmd: MMUEXT_TLB_FLUSH_LOCAL
 * No additional arguments. Flushes local TLB.
 * 
 * cmd: MMUEXT_INVLPG_LOCAL
 * linear_addr: Linear address to be flushed from the local TLB.
 * 
 * cmd: MMUEXT_TLB_FLUSH_MULTI
 * cpuset: Pointer to bitmap of VCPUs to be flushed.
 * 
 * cmd: MMUEXT_INVLPG_MULTI
 * linear_addr: Linear address to be flushed.
 * cpuset: Pointer to bitmap of VCPUs to be flushed.
 * 
 * cmd: MMUEXT_TLB_FLUSH_ALL
 * No additional arguments. Flushes all VCPUs' TLBs.
 * 
 * cmd: MMUEXT_INVLPG_ALL
 * linear_addr: Linear address to be flushed from all VCPUs' TLBs.
 * 
 * cmd: MMUEXT_FLUSH_CACHE
 * No additional arguments. Writes back and flushes cache contents.
 * 
 * cmd: MMUEXT_SET_LDT
 * linear_addr: Linear address of LDT base (NB. must be page-aligned).
 * nr_ents: Number of entries in LDT.
 * 
 * cmd: MMUEXT_REASSIGN_PAGE
 * mfn: Machine frame number to be reassigned to the FD.
 *      (NB. page must currently belong to the calling domain).
 */
#define MMUEXT_PIN_L1_TABLE      0
#define MMUEXT_PIN_L2_TABLE      1
#define MMUEXT_PIN_L3_TABLE      2
#define MMUEXT_PIN_L4_TABLE      3
#define MMUEXT_UNPIN_TABLE       4
#define MMUEXT_NEW_BASEPTR       5
#define MMUEXT_TLB_FLUSH_LOCAL   6
#define MMUEXT_INVLPG_LOCAL      7
#define MMUEXT_TLB_FLUSH_MULTI   8
#define MMUEXT_INVLPG_MULTI      9
#define MMUEXT_TLB_FLUSH_ALL    10
#define MMUEXT_INVLPG_ALL       11
#define MMUEXT_FLUSH_CACHE      12
#define MMUEXT_SET_LDT          13
#define MMUEXT_REASSIGN_PAGE    14
#define MMUEXT_NEW_USER_BASEPTR 15

#ifndef __ASSEMBLY__
struct mmuext_op {
    unsigned int cmd;
    union {
        /* [UN]PIN_TABLE, NEW_BASEPTR, NEW_USER_BASEPTR, REASSIGN_PAGE */
        memory_t mfn;
        /* INVLPG_LOCAL, INVLPG_ALL, SET_LDT */
        memory_t linear_addr;
    };
    union {
        /* SET_LDT */
        unsigned int nr_ents;
        /* TLB_FLUSH_MULTI, INVLPG_MULTI */
        void *cpuset;
    };
};
#endif

/* These are passed as 'flags' to update_va_mapping. They can be ORed. */
/* When specifying UVMF_MULTI, also OR in a pointer to a CPU bitmap.   */
/* UVMF_LOCAL is merely UVMF_MULTI with a NULL bitmap pointer.         */
#define UVMF_NONE               (0UL)    /* No flushing at all.   */
#define UVMF_TLB_FLUSH          (1UL<<0) /* Flush entire TLB(s).  */
#define UVMF_INVLPG             (2UL<<0) /* Flush only one entry. */
#define UVMF_FLUSHTYPE_MASK     (3UL<<0)
#define UVMF_MULTI              (0UL<<1) /* Flush subset of TLBs. */
#define UVMF_LOCAL              (0UL<<2) /* Flush local TLB.      */
#define UVMF_ALL                (1UL<<2) /* Flush all TLBs.       */

/*
 * Commands to HYPERVISOR_sched_op().
 */
#define SCHEDOP_yield           0   /* Give up the CPU voluntarily.       */
#define SCHEDOP_block           1   /* Block until an event is received.  */
#define SCHEDOP_shutdown        2   /* Stop executing this domain.        */
#define SCHEDOP_cmdmask       255   /* 8-bit command. */
#define SCHEDOP_reasonshift     8   /* 8-bit reason code. (SCHEDOP_shutdown) */

/*
 * Reason codes for SCHEDOP_shutdown. These may be interpreted by control 
 * software to determine the appropriate action. For the most part, Xen does
 * not care about the shutdown code (SHUTDOWN_crash excepted).
 */
#define SHUTDOWN_poweroff   0  /* Domain exited normally. Clean up and kill. */
#define SHUTDOWN_reboot     1  /* Clean up, kill, and then restart.          */
#define SHUTDOWN_suspend    2  /* Clean up, save suspend info, kill.         */
#define SHUTDOWN_crash      3  /* Tell controller we've crashed.             */

/*
 * Commands to HYPERVISOR_console_io().
 */
#define CONSOLEIO_write         0
#define CONSOLEIO_read          1

/*
 * Commands to HYPERVISOR_dom_mem_op().
 */
#define MEMOP_increase_reservation 0
#define MEMOP_decrease_reservation 1

/*
 * Commands to HYPERVISOR_vm_assist().
 */
#define VMASST_CMD_enable                0
#define VMASST_CMD_disable               1
#define VMASST_TYPE_4gb_segments         0
#define VMASST_TYPE_4gb_segments_notify  1
#define VMASST_TYPE_writable_pagetables  2
#define MAX_VMASST_TYPE 2

#ifndef __ASSEMBLY__

typedef u16 domid_t;

/* Domain ids >= DOMID_FIRST_RESERVED cannot be used for ordinary domains. */
#define DOMID_FIRST_RESERVED (0x7FF0U)

/* DOMID_SELF is used in certain contexts to refer to oneself. */
#define DOMID_SELF (0x7FF0U)

/*
 * DOMID_IO is used to restrict page-table updates to mapping I/O memory.
 * Although no Foreign Domain need be specified to map I/O pages, DOMID_IO
 * is useful to ensure that no mappings to the OS's own heap are accidentally
 * installed. (e.g., in Linux this could cause havoc as reference counts
 * aren't adjusted on the I/O-mapping code path).
 * This only makes sense in MMUEXT_SET_FOREIGNDOM, but in that context can
 * be specified by any calling domain.
 */
#define DOMID_IO   (0x7FF1U)

/*
 * DOMID_XEN is used to allow privileged domains to map restricted parts of
 * Xen's heap space (e.g., the machine_to_phys table).
 * This only makes sense in MMUEXT_SET_FOREIGNDOM, and is only permitted if
 * the caller is privileged.
 */
#define DOMID_XEN  (0x7FF2U)

/*
 * Send an array of these to HYPERVISOR_mmu_update().
 * NB. The fields are natural pointer/address size for this architecture.
 */
typedef struct
{
    memory_t ptr;       /* Machine address of PTE. */
    memory_t val;       /* New contents of PTE.    */
} PACKED mmu_update_t;

/*
 * Send an array of these to HYPERVISOR_multicall().
 * NB. The fields are natural register size for this architecture.
 */
typedef struct
{
    cpureg_t op;
    cpureg_t args[7];
} PACKED multicall_entry_t;

/* Event channel endpoints per domain. */
#define NR_EVENT_CHANNELS 1024

/* Support for multi-processor guests. */
#define MAX_VIRT_CPUS 32

/*
 * Per-VCPU information goes here. This will be cleaned up more when Xen 
 * actually supports multi-VCPU guests.
 */
typedef struct
{
    /*
     * 'evtchn_upcall_pending' is written non-zero by Xen to indicate
     * a pending notification for a particular VCPU. It is then cleared 
     * by the guest OS /before/ checking for pending work, thus avoiding
     * a set-and-check race. Note that the mask is only accessed by Xen
     * on the CPU that is currently hosting the VCPU. This means that the
     * pending and mask flags can be updated by the guest without special
     * synchronisation (i.e., no need for the x86 LOCK prefix).
     * This may seem suboptimal because if the pending flag is set by
     * a different CPU then an IPI may be scheduled even when the mask
     * is set. However, note:
     *  1. The task of 'interrupt holdoff' is covered by the per-event-
     *     channel mask bits. A 'noisy' event that is continually being
     *     triggered can be masked at source at this very precise
     *     granularity.
     *  2. The main purpose of the per-VCPU mask is therefore to restrict
     *     reentrant execution: whether for concurrency control, or to
     *     prevent unbounded stack usage. Whatever the purpose, we expect
     *     that the mask will be asserted only for short periods at a time,
     *     and so the likelihood of a 'spurious' IPI is suitably small.
     * The mask is read before making an event upcall to the guest: a
     * non-zero mask therefore guarantees that the VCPU will not receive
     * an upcall activation. The mask is cleared when the VCPU requests
     * to block: this avoids wakeup-waiting races.
     */
    u8 evtchn_upcall_pending;           /* 0 */
    u8 evtchn_upcall_mask;              /* 1 */
    u8 pad0, pad1;
    u32 evtchn_pending_sel;             /* 4 */
    arch_vcpu_info_t arch;              /* 8 */
} PACKED vcpu_info_t;                   /* 8 + arch */

/*
 * Xen/kernel shared data -- pointer provided in start_info.
 * NB. We expect that this struct is smaller than a page.
 */
typedef struct shared_info_st
{
    vcpu_info_t vcpu_data[MAX_VIRT_CPUS];  /*   0 */

    u32 n_vcpu;

    /*
     * A domain can have up to 1024 "event channels" on which it can send
     * and receive asynchronous event notifications. There are three classes
     * of event that are delivered by this mechanism:
     *  1. Bi-directional inter- and intra-domain connections. Domains must
     *     arrange out-of-band to set up a connection (usually the setup
     *     is initiated and organised by a privileged third party such as
     *     software running in domain 0).
     *  2. Physical interrupts. A domain with suitable hardware-access
     *     privileges can bind an event-channel port to a physical interrupt
     *     source.
     *  3. Virtual interrupts ('events'). A domain can bind an event-channel
     *     port to a virtual interrupt source, such as the virtual-timer
     *     device or the emergency console.
     * 
     * Event channels are addressed by a "port index" between 0 and 1023.
     * Each channel is associated with two bits of information:
     *  1. PENDING -- notifies the domain that there is a pending notification
     *     to be processed. This bit is cleared by the guest.
     *  2. MASK -- if this bit is clear then a 0->1 transition of PENDING
     *     will cause an asynchronous upcall to be scheduled. This bit is only
     *     updated by the guest. It is read-only within Xen. If a channel
     *     becomes pending while the channel is masked then the 'edge' is lost
     *     (i.e., when the channel is unmasked, the guest must manually handle
     *     pending notifications as no upcall will be scheduled by Xen).
     * 
     * To expedite scanning of pending notifications, any 0->1 pending
     * transition on an unmasked channel causes a corresponding bit in a
     * 32-bit selector to be set. Each bit in the selector covers a 32-bit
     * word in the PENDING bitfield array.
     */
    u32 evtchn_pending[32];             /*   4 */
    u32 evtchn_mask[32];                /* 136 */

    /*
     * Time: The following abstractions are exposed: System Time, Clock Time,
     * Domain Virtual Time. Domains can access Cycle counter time directly.
     */
    u64                cpu_freq;        /* 264: CPU frequency (Hz).          */

    /*
     * The following values are updated periodically (and not necessarily
     * atomically!). The guest OS detects this because 'time_version1' is
     * incremented just before updating these values, and 'time_version2' is
     * incremented immediately after. See the Xen-specific Linux code for an
     * example of how to read these values safely (arch/xen/kernel/time.c).
     */
    u32                time_version1;   /* 272 */
    u32                time_version2;   /* 276 */
    tsc_timestamp_t    tsc_timestamp;   /* TSC at last update of time vals.  */
    u64                system_time;     /* Time, in nanosecs, since boot.    */
    u32                wc_sec;          /* Secs  00:00:00 UTC, Jan 1, 1970.  */
    u32                wc_usec;         /* Usecs 00:00:00 UTC, Jan 1, 1970.  */
    u64                domain_time;     /* Domain virtual time, in nanosecs. */

    /*
     * Timeout values:
     * Allow a domain to specify a timeout value in system time and 
     * domain virtual time.
     */
    u64                wall_timeout;    /* 312 */
    u64                domain_timeout;  /* 320 */

    arch_shared_info_t arch;

} PACKED shared_info_t;

/*
 * Start-of-day memory layout for the initial domain (DOM0):
 *  1. The domain is started within contiguous virtual-memory region.
 *  2. The contiguous region begins and ends on an aligned 4MB boundary.
 *  3. The region start corresponds to the load address of the OS image.
 *     If the load address is not 4MB aligned then the address is rounded down.
 *  4. This the order of bootstrap elements in the initial virtual region:
 *      a. relocated kernel image
 *      b. initial ram disk              [mod_start, mod_len]
 *      c. list of allocated page frames [mfn_list, nr_pages]
 *      d. bootstrap page tables         [pt_base, CR3 (x86)]
 *      e. start_info_t structure        [register ESI (x86)]
 *      f. bootstrap stack               [register ESP (x86)]
 *  5. Bootstrap elements are packed together, but each is 4kB-aligned.
 *  6. The initial ram disk may be omitted.
 *  7. The list of page frames forms a contiguous 'pseudo-physical' memory
 *     layout for the domain. In particular, the bootstrap virtual-memory
 *     region is a 1:1 mapping to the first section of the pseudo-physical map.
 *  8. All bootstrap elements are mapped read-writable for the guest OS. The
 *     only exception is the bootstrap page table, which is mapped read-only.
 *  9. There is guaranteed to be at least 512kB padding after the final
 *     bootstrap element. If necessary, the bootstrap virtual region is
 *     extended by an extra 4MB to ensure this.
 */

#define MAX_CMDLINE 256
typedef struct {
    /* THE FOLLOWING ARE FILLED IN BOTH ON INITIAL BOOT AND ON RESUME.     */
    memory_t nr_pages;        /*  0: Total pages allocated to this domain. */
    _MEMORY_PADDING(A);
    memory_t shared_info;     /*  8: MACHINE address of shared info struct.*/
    _MEMORY_PADDING(B);
    u32      flags;           /* 16: SIF_xxx flags.                        */
    u16      domain_controller_evtchn; /* 20 */
    u16      __pad;
    /* THE FOLLOWING ARE ONLY FILLED IN ON INITIAL BOOT (NOT RESUME).      */
    memory_t pt_base;         /* 24: VIRTUAL address of page directory.    */
    _MEMORY_PADDING(C);
    memory_t nr_pt_frames;    /* 32: Number of bootstrap p.t. frames.      */
    _MEMORY_PADDING(D);
    memory_t mfn_list;        /* 40: VIRTUAL address of page-frame list.   */
    _MEMORY_PADDING(E);
    memory_t mod_start;       /* 48: VIRTUAL address of pre-loaded module. */
    _MEMORY_PADDING(F);
    memory_t mod_len;         /* 56: Size (bytes) of pre-loaded module.    */
    _MEMORY_PADDING(G);
    s8 cmd_line[MAX_CMDLINE]; /* 64 */
} PACKED start_info_t; /* 320 bytes */

/* These flags are passed in the 'flags' field of start_info_t. */
#define SIF_PRIVILEGED    (1<<0)  /* Is the domain privileged? */
#define SIF_INITDOMAIN    (1<<1)  /* Is this the initial control domain? */
#define SIF_BLK_BE_DOMAIN (1<<4)  /* Is this a block backend domain? */
#define SIF_NET_BE_DOMAIN (1<<5)  /* Is this a net backend domain? */
#define SIF_USB_BE_DOMAIN (1<<6)  /* Is this a usb backend domain? */
/* For use in guest OSes. */
extern shared_info_t *HYPERVISOR_shared_info;

#endif /* !__ASSEMBLY__ */

#endif /* __XEN_PUBLIC_XEN_H__ */