#ifndef _ASM_ARM_UNISTD_NR_H
#define _ASM_ARM_UNISTD_NR_H 1

/*
 * This needs to be greater than __NR_last_syscall+1 in order to account
 * for the padding in the syscall table.
 */

/* aligned to 4 */
#define __NR_syscalls 436

#endif /* _ASM_ARM_UNISTD_NR_H */
