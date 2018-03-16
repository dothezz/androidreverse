#include "ptrace_util.h"

#define CPSR_T_MASK (1u << 5)

long ptrace_retval(struct pt_regs *regs) { return regs->ARM_r0; }

long ptrace_ip(struct pt_regs *regs) { return regs->ARM_pc; }

int ptrace_readdata(pid_t pid, const uint8_t *src, const uint8_t *buf,
                    size_t size) {
    uint32_t i, j, remain;
    uint8_t *laddr;

    union u {
        long val;
        char chars[sizeof(long)];
    } d;

    j = size / 4;
    remain = size % 4;
    laddr = buf;

    for (i = 0; i < j; i++) {
        d.val = ptrace(PTRACE_PEEKTEXT, pid, src, 0);
        memcpy(laddr, d.chars, 4);
        src += 4;
        laddr += 4;
    }

    if (remain > 0) {
        d.val = ptrace(PTRACE_PEEKTEXT, pid, src, 0);
        memcpy(laddr, d.chars, remain);
    }

    return 0;
}

int ptrace_writedata(pid_t pid, const uint8_t *dest, const uint8_t *data,
                     size_t size) {
    uint32_t i, j, remain;
    const uint8_t *laddr;

    union u {
        long val;
        char chars[sizeof(long)];
    } d;

    j = size / 4;
    remain = size % 4;

    laddr = data;

    for (i = 0; i < j; i++) {
        memcpy(d.chars, laddr, 4);
        ptrace(PTRACE_POKETEXT, pid, dest, d.val);
        dest += 4;
        laddr += 4;
    }

    if (remain > 0) {
        d.val = ptrace(PTRACE_PEEKTEXT, pid, dest, 0);
        for (i = 0; i < remain; i++) {
            d.chars[i] = *laddr++;
        }

        ptrace(PTRACE_POKETEXT, pid, dest, d.val);
    }

    return 0;
}

int ptrace_call(pid_t pid, uint32_t addr, const long *params,
                uint32_t num_params, struct pt_regs *regs) {
    uint32_t i;
    // first 4 params use register
    for (i = 0; i < num_params && i < 4; i++) {
        regs->uregs[i] = params[i];
    }

    // more params use stack
    if (i < num_params) {
        regs->ARM_sp -= (num_params - i) * sizeof(long);
        ptrace_writedata(pid, (void *)regs->ARM_sp, (uint8_t *)&params[i],
                         (num_params - i) * sizeof(long));
    }

    // change pc regs content
    regs->ARM_pc = addr;

    if (regs->ARM_pc & 1) {
        /* thumb */
        regs->ARM_pc &= (~1u);
        regs->ARM_cpsr |= CPSR_T_MASK;
    } else {
        /* arm */
        regs->ARM_cpsr &= ~CPSR_T_MASK;
    }

    // set stack ret addr with 0, call complete ret addr is error, local process get control
    regs->ARM_lr = 0;

    // reload backup registers and continue it
    if (ptrace_setregs(pid, regs) == -1 || ptrace_continue(pid) == -1) {
        return -1;
    }
    // wait process exit
    int stat = 0;
    waitpid(pid, &stat, WUNTRACED);
    while (stat != 0xb7f) {
        if (ptrace_continue(pid) == -1) {
            return -1;
        }
        waitpid(pid, &stat, WUNTRACED);
    }

    return 0;
}

int ptrace_getregs(pid_t pid, const struct pt_regs *regs) {
    if (ptrace(PTRACE_GETREGS, pid, NULL, regs) < 0) {
        return -1;
    }

    return 0;
}

int ptrace_setregs(pid_t pid, const struct pt_regs *regs) {
    if (ptrace(PTRACE_SETREGS, pid, NULL, regs) < 0) {
        perror("ptrace_setregs: Can not set register values");
        return -1;
    }

    return 0;
}

int ptrace_continue(pid_t pid) {
    if (ptrace(PTRACE_CONT, pid, NULL, 0) < 0) {
        perror("ptrace_cont");
        return -1;
    }

    return 0;
}

int ptrace_attach(pid_t pid) {
    if (ptrace(PTRACE_ATTACH, pid, NULL, 0) < 0) {
        return -1;
    }

    int status = 0;
    waitpid(pid, &status, WUNTRACED);

    return 0;
}

int ptrace_detach(pid_t pid) {
    if (ptrace(PTRACE_DETACH, pid, NULL, 0) < 0) {
        perror("ptrace_detach");
        return -1;
    }

    return 0;
}

int ptrace_call_wrapper(pid_t target_pid, const char *func_name,
                        void *func_addr, long *parameters, int param_num,
                        struct pt_regs *regs) {
    LOGD("Calling [%s] in target process <%d> \n", func_name, target_pid);
    if (ptrace_call(target_pid, (uint32_t)func_addr, parameters, param_num,
                    regs) < 0) {
        return -1;
    }

    if (ptrace_getregs(target_pid, regs) < 0) {
        return -1;
    }

    LOGD("[+] Target process returned from %s, return value=%x, pc=%x \n",
         func_name, ptrace_retval(regs), ptrace_ip(regs));
    return 0;
}
