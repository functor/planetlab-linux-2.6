/*
 * Copyright (C) 2003-2004 Intel
 * Copyright (C) Tom Long Nguyen (tom.l.nguyen@intel.com)
 */

#ifndef ASM_MSI_H
#define ASM_MSI_H

#include <asm/desc.h>

#define LAST_DEVICE_VECTOR		232
#define MSI_DEST_MODE			MSI_LOGICAL_MODE
#define MSI_TARGET_CPU_SHIFT		12

#ifdef CONFIG_SMP
#define MSI_TARGET_CPU		logical_smp_processor_id()
#else
#define MSI_TARGET_CPU		TARGET_CPUS
#endif

#endif /* ASM_MSI_H */
