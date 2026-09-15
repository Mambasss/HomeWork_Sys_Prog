#define _GNU_SOURCE
#include <signal.h>
#include <ucontext.h>
#include <stdio.h>
#include <stdlib.h>
#include "sigfpe.h"
#include "zydis/Zydis.h"

static ZydisDecoder g_decoder;
static int g_decoder_ready = 0;

static void sigfpe_handler(int sig, siginfo_t *info, void *uctx)
{
    (void)sig; (void)info;
    ucontext_t *uc = (ucontext_t *)uctx;
    greg_t     *regs = uc->uc_mcontext.gregs;
    unsigned char *rip = (unsigned char *)regs[REG_RIP];

    if (!g_decoder_ready) {
        ZydisDecoderInit(&g_decoder,
                         ZYDIS_MACHINE_MODE_LONG_64,
                         ZYDIS_STACK_WIDTH_64);
        g_decoder_ready = 1;
    }

    ZydisDecodedInstruction insn;
    if (ZYAN_SUCCESS(ZydisDecoderDecodeInstruction(
            &g_decoder, NULL, rip, ZYDIS_MAX_INSTRUCTION_LENGTH, &insn)) &&
        (insn.mnemonic == ZYDIS_MNEMONIC_DIV ||
         insn.mnemonic == ZYDIS_MNEMONIC_IDIV))
    {
        regs[REG_RAX] = 0;
        regs[REG_RDX] = 0;
        regs[REG_RIP] = (greg_t)(rip + insn.length);
        return;
    }

    fprintf(stderr, "[sigfpe] unhandled #DE at %p (bytes %02x %02x %02x %02x)\n",
            rip, rip[0], rip[1], rip[2], rip[3]);
    signal(SIGFPE, SIG_DFL);
}

void install_sigfpe_handler(void)
{
    struct sigaction sa = {0};
    sa.sa_sigaction = sigfpe_handler;
    sa.sa_flags     = SA_SIGINFO | SA_RESTART;
    sigemptyset(&sa.sa_mask);
    if (sigaction(SIGFPE, &sa, NULL) != 0) {
        perror("sigaction");
        exit(1);
    }
}