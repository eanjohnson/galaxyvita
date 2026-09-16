#include "flipper.h"

#include "cpu/cpu.h"
#include "gx_cp.h"
#include "ios.h"

#include <string.h>

#define PI_CAUSE_VI 0x100u
#define PI_CAUSE_DSP 0x40u
#define PI_CAUSE_RST 0x10000u
#define PI_CAUSE_IPC 0x4000u

/* Broadway's half of the IPC control register. */
#define IPC_CTRL_X1 0x01u
#define IPC_CTRL_Y2 0x02u
#define IPC_CTRL_Y1 0x04u
#define IPC_CTRL_X2 0x08u
#define IPC_CTRL_IY1 0x10u
#define IPC_CTRL_IY2 0x20u
#define PI_CAUSE_PE_TOKEN 0x200u
#define PI_CAUSE_PE_FINISH 0x400u
#define PI_CAUSE_AI 0x20u
#define DSP_INT 0x80u

/* Audio Interface, 0xCD006C00. Four registers: control, volume, the free-
 * running sample counter and the counter value the interrupt fires at. */
#define AI_CR 0u
#define AI_SCNT 2u
#define AI_IT 3u
#define AICR_PSTAT 0x0001u    /* sample counter runs */
#define AICR_AFR 0x0002u      /* 0 = 32 kHz, 1 = 48 kHz */
#define AICR_AIINTMSK 0x0004u
#define AICR_AIINT 0x0008u    /* write one to clear */
#define AICR_AIINTVLD 0x0010u
#define AICR_SCRESET 0x0020u  /* a command, not a bit: never reads back set */
/* The counter is in sample-rate units and the chassis counts Broadway cycles,
 * so the conversion runs through the core clock. */
#define AI_CPU_HZ 729000000u

#define VI_VCOUNT_OFF 0x2Cu
#define VI_HCOUNT_OFF 0x2Eu
#define VI_DI0_OFF 0x30u
#define VI_NTSC_LINES 525u
/* One NTSC scanline is 1/(59.94*525) s. Broadway runs at 729 MHz, and the
 * chassis counts dispatch cycles in those units, so a line is this many of
 * them. Getting this right matters: drive VI off block count instead and the
 * game's frame clock races thousands of times ahead of the code feeding it,
 * which starves every thread that waits on a retrace. */
#define VI_CYCLES_PER_LINE 23170u

/* Task-management mails, as the DSP boot ROM and Nintendo's ucodes use them.
 * The DSP reports status with 0xDCD1 in the high word; the CPU answers with
 * 0xCDD1. See Dolphin's Source/Core/Core/HW/DSPHLE/UCodes/UCodes.h. */
#define DSP_INIT_MAIL 0xDCD10000u
/* The boot ROM's greeting. __DSP_boot_task waits for exactly this before it
 * starts feeding the ucode-upload sequence. */
#define DSP_ROM_HELLO 0x8071FEEDu
/* The init ucode's greeting, which __OSInitAudioSystem waits for. */
#define DSP_INIT_UCODE_HELLO 0x80544348u
/* SMG runs its own ucode and talks to it with 0xF355-tagged mails (see the
 * game's DSP callback at 0x80494CC0). We do not run DSP code, so the ucode's
 * greeting is synthesised; only its tag matters to the callers. */
#define DSP_UCODE_HELLO 0xF3550000u
/* DSP_SYNC, the task mail the handler at 0x804965A0 routes to that callback. */
#define DSP_TASK_SYNC 0xDCD10004u
#define DSP_BOOT_SEL_MASK 0xFFFF0000u
#define DSP_BOOT_SEL 0x80F30000u
#define DSP_BOOT_RAM_ADDR 0x80F3A001u
#define DSP_BOOT_LENGTH 0x80F3A002u
#define DSP_BOOT_DMEM_LENGTH 0x80F3B002u
#define DSP_BOOT_IMEM_ADDR 0x80F3C002u
#define DSP_BOOT_START_PC 0x80F3D001u

static void note_ea(Flipper *f, u32 ea) {
  u32 key = ea & ~3u;
  if (f->unique_n >= FLIPPER_MMIO_TRACE)
    return;
  for (u32 i = 0; i < f->unique_n; i++) {
    if (f->unique_ea[i] == key)
      return;
  }
  f->unique_ea[f->unique_n++] = key;
}

static int classify(u32 ea, u32 *off, int *hollywood) {
  u32 top = ea >> 24;
  if (top == 0xCCu || top == 0x0Cu) {
    *hollywood = 0;
    *off = ea & 0x00FFFFFFu;
    return 1;
  }
  if (top == 0xCDu || top == 0x0Du) {
    *hollywood = 1;
    *off = ea & 0x00FFFFFFu;
    if (*off >= 0x800000u)
      *off -= 0x800000u; /* 0x0D800000 AHB window */
    return 1;
  }
  return 0;
}

int flipper_is_mmio(u32 ea) {
  u32 off;
  int hw;
  return classify(ea, &off, &hw);
}

/* Pixel Engine control register, at 0xCC00100A. The low two bits enable the
 * token and finish interrupts; the next two acknowledge them and do not stick.
 * The last draw-sync token reads back from 0xCC00100E. */
#define PE_CTRL_OFF 0x0Au
#define PE_TOKEN_OFF 0x0Eu
#define PE_CTRL_TOKEN_ENABLE 0x1u
#define PE_CTRL_FINISH_ENABLE 0x2u
#define PE_CTRL_TOKEN_ACK 0x4u
#define PE_CTRL_FINISH_ACK 0x8u

static void pe_update_irq(Flipper *f) {
  u16 ctrl = f->pe[PE_CTRL_OFF / 2];
  if (f->pe_token_irq && (ctrl & PE_CTRL_TOKEN_ENABLE))
    f->pi_cause |= PI_CAUSE_PE_TOKEN;
  else
    f->pi_cause &= ~PI_CAUSE_PE_TOKEN;
  if (f->pe_finish_irq && (ctrl & PE_CTRL_FINISH_ENABLE))
    f->pi_cause |= PI_CAUSE_PE_FINISH;
  else
    f->pi_cause &= ~PI_CAUSE_PE_FINISH;
}

/* Retire the PE events the command parser found. There is no GPU pipeline
 * behind the FIFO here, so a draw-done token completes the moment the CPU
 * pushes it -- which is all GXDrawDone needs to stop sleeping. */
static void pe_drain(Flipper *f, GxCp *gx) {
  u32 token;
  if (gx_cp_take_token(gx, &token))
    f->pe[PE_TOKEN_OFF / 2] = (u16)token;
  if (gx_cp_take_token_irq(gx))
    f->pe_token_irq = 1;
  if (gx_cp_take_finish(gx))
    f->pe_finish_irq = 1;
  pe_update_irq(f);
}

static void dsp_update_irq(Flipper *f) {
  u16 csr = f->dsp[0x0A / 2];
  int pending = ((csr >> 1) & csr & (DSP_INT | 0x20u | 0x08u)) != 0;
  if (pending)
    f->pi_cause |= PI_CAUSE_DSP;
  else
    f->pi_cause &= ~PI_CAUSE_DSP;
}

#define DSP_CSR_RESET 0x0001u
#define DSP_CSR_HALT 0x0004u
#define DSP_CSR_STATUS_BITS 0x00A8u /* AID | ARAM | DSP, all write-1-to-clear */
#define DSP_CSR_INIT_CODE 0x0400u
#define DSP_CSR_INIT 0x0800u

static void dsp_push_mail(Flipper *f, u32 mail, int irq) {
  u32 next = (f->dsp_mail_tail + 1u) % 8u;
  if (next == f->dsp_mail_head)
    return; /* ring full: the CPU is not draining, so the mail is moot */
  f->dsp_mail[f->dsp_mail_tail] = mail;
  f->dsp_mail_tail = next;
  if (irq) {
    f->dsp[0x0A / 2] |= (u16)DSP_INT;
    dsp_update_irq(f);
  }
}

/* The CPU reads the high half to test the valid bit and the low half to retire
 * the mail, so both halves work off the same latch. */
static u32 dsp_latch_mail(Flipper *f, int retire) {
  if (!f->dsp_halted && f->dsp_mail_head != f->dsp_mail_tail) {
    f->dsp_last_mail = f->dsp_mail[f->dsp_mail_head];
    if (retire)
      f->dsp_mail_head = (f->dsp_mail_head + 1u) % 8u;
  }
  return f->dsp_last_mail;
}

/* Reset drops the DSP back into its boot ROM, which greets the CPU. */
static void dsp_reset(Flipper *f) {
  f->dsp_mail_head = f->dsp_mail_tail = 0u;
  f->dsp_last_mail = 0u;
  f->dsp_boot_sel = 0u;
  f->dsp_ucode_loaded = 0;
  f->dsp_cmd_left = 0u;
  dsp_push_mail(f, DSP_ROM_HELLO, 0);
}

/* One CPU->DSP mail, handled the way the IROM boot ucode does: collect the
 * 0x80F3xxxx selector/value pairs that describe the ucode, and treat the start
 * address as "go". We do not run DSP code, so booting a ucode means switching
 * to the task-mail protocol the SDK's DSP scheduler expects. */
static void dsp_handle_mail(Flipper *f, u32 mail) {
  if (f->dsp_ucode_loaded) {
    /* SMG drives its ucode with a word count followed by that many words (see
     * the sender at 0x804962C0). The first word tags the command, and the game
     * matches the reply by that tag's high half -- its dispatcher at 0x804964A0
     * compares mail & 0xFFFF against the tags in submission order, so every
     * command must be answered exactly once and in order. We run no DSP code,
     * so the reply is all there is. */
    if (f->dsp_cmd_left == 0u) {
      f->dsp_cmd_left = mail & 0xFFFFu;
      f->dsp_cmd_seen = 0u;
      return;
    }
    if (f->dsp_cmd_seen == 0u)
      f->dsp_cmd_tag = mail;
    f->dsp_cmd_seen++;
    f->dsp_cmd_left--;
    if (f->dsp_cmd_left == 0u) {
      /* The ucode reports a completed command as DSP_SYNC; the game's DSP
       * interrupt handler then hands over to the callback at task+52, which
       * reads one more mail and matches it against the tags it registered. */
      dsp_push_mail(f, DSP_TASK_SYNC, 1);
      dsp_push_mail(f, DSP_UCODE_HELLO | (f->dsp_cmd_tag >> 16), 0);
    }
    return;
  }
  if (f->dsp_boot_sel == 0u) {
    if ((mail & DSP_BOOT_SEL_MASK) != DSP_BOOT_SEL)
      dsp_push_mail(f, 0xFEEE0000u | (mail & 0xFFFFu), 0);
    else
      f->dsp_boot_sel = mail;
    return;
  }
  switch (f->dsp_boot_sel) {
  case DSP_BOOT_RAM_ADDR:
    f->dsp_ucode_addr = mail;
    break;
  case DSP_BOOT_LENGTH:
    f->dsp_ucode_len = mail & 0xFFFFu;
    break;
  case DSP_BOOT_IMEM_ADDR:
  case DSP_BOOT_DMEM_LENGTH:
    break;
  case DSP_BOOT_START_PC:
    f->dsp_boot_sel = 0u;
    f->dsp_ucode_loaded = 1;
    /* A freshly started ucode reports DSP_INIT to the SDK's task scheduler, and
     * the scheduler hands control to the task's init callback. SMG's callback
     * then does its own handshake with its ucode and waits for one more mail,
     * so the greeting follows immediately. */
    dsp_push_mail(f, DSP_INIT_MAIL, 1);
    dsp_push_mail(f, DSP_UCODE_HELLO, 0);
    return;
  default:
    break;
  }
  f->dsp_boot_sel = 0u;
}

static void dsp_write_csr(Flipper *f, u16 want) {
  u16 prev = f->dsp[0x0A / 2];
  u16 csr = (u16)((want & ~DSP_CSR_STATUS_BITS) |
                  (prev & ~want & DSP_CSR_STATUS_BITS));
  csr &= (u16)~DSP_CSR_RESET; /* hardware clears reset once the DSP is back */
  f->dsp[0x0A / 2] = csr;
  f->dsp_halted = (csr & DSP_CSR_HALT) != 0;
  if (want & DSP_CSR_RESET)
    dsp_reset(f);
  /* Clearing DSPInit runs the small init ucode the SDK's __OSInitAudioSystem
   * waits on; DSPInitCode reads back set until the DSP has picked it up. */
  if ((prev & DSP_CSR_INIT) && !(want & DSP_CSR_INIT)) {
    f->dsp_mail_head = f->dsp_mail_tail = 0u;
    f->dsp_last_mail = 0u;
    f->dsp[0x0A / 2] |= (u16)DSP_CSR_INIT_CODE;
    dsp_push_mail(f, DSP_INIT_UCODE_HELLO, 0);
  }
  dsp_update_irq(f);
}

/* ARAM DMA. The Wii has no separate aux RAM: Hollywood keeps the GameCube's
 * ARAM DMA engine but points it at MEM2, so a transfer is a copy between a MEM1
 * physical address and a MEM2 offset (Dolphin does the same -- see
 * Source/Core/Core/HW/DSP.cpp, wii_mode). Real hardware takes ~246 cycles per
 * 32 bytes; we finish inside the register write and raise ARINT, which is all
 * the SDK's ARQ queue looks at. */
#define AR_DMA_DIR_TO_MRAM 0x80000000u
#define AR_ADDR_MASK 0x03FFFFFFu
#define DSP_CSR_ARINT 0x20u

static void aram_dma(Flipper *f, CPUState *cpu) {
  u32 mm = ((u32)f->dsp[0x20 / 2] << 16) | f->dsp[0x22 / 2];
  u32 ar = ((u32)f->dsp[0x24 / 2] << 16) | f->dsp[0x26 / 2];
  u32 cnt = ((u32)f->dsp[0x28 / 2] << 16) | f->dsp[0x2A / 2];

  int to_mram = (cnt & AR_DMA_DIR_TO_MRAM) != 0;
  u32 len = cnt & ~AR_DMA_DIR_TO_MRAM;
  u32 mm_ea = 0x80000000u | (mm & AR_ADDR_MASK);
  u32 ar_ea = 0x90000000u + (ar & AR_ADDR_MASK);
  if (cpu && cpu->mem2) {
    for (u32 i = 0; i + 4u <= len; i += 4u) {
      if (to_mram)
        mem_write32(cpu, mm_ea + i, mem_read32(cpu, ar_ea + i));
      else
        mem_write32(cpu, ar_ea + i, mem_read32(cpu, mm_ea + i));
    }
  }
  f->dsp[0x0A / 2] |= (u16)DSP_CSR_ARINT;
  dsp_update_irq(f);
}

void flipper_init(Flipper *f) {
  memset(f, 0, sizeof(*f));
  f->pi_cause = PI_CAUSE_RST;
  f->cp[0] = 0x000C; /* ReadIdle | CommandIdle */
  dsp_reset(f);
  f->hw_resets = 0xFFFFFFFFu;
  f->ipc_irq_mask = 0x40000000u;
  f->vcount = 1;
  f->hcount = 1;
}


/* The sample-counter interrupt is the one AIRegisterStreamCallback uses. The
 * status bit latches on its own and the mask only gates delivery, so both are
 * re-derived here rather than assumed at the point either one moves. */
static void sync_ai_irq(Flipper *f) {
  if ((f->ai[AI_CR] & AICR_AIINT) && (f->ai[AI_CR] & AICR_AIINTMSK))
    f->pi_cause |= PI_CAUSE_AI;
  else
    f->pi_cause &= ~PI_CAUSE_AI;
}

static void ai_advance(Flipper *f, u64 cycles) {
  if (!(f->ai[AI_CR] & AICR_PSTAT))
    return;
  u32 rate = (f->ai[AI_CR] & AICR_AFR) ? 48000u : 32000u;
  f->ai_acc += cycles * rate;
  u32 ticks = (u32)(f->ai_acc / AI_CPU_HZ);
  if (!ticks)
    return;
  f->ai_acc -= (u64)ticks * AI_CPU_HZ;
  u32 before = f->ai_count;
  f->ai_count += ticks;
  /* AIIT is a one-shot: the interrupt fires on the crossing, not while the
   * counter is past it. A zero AIIT is the SDK's "no callback" state. */
  if (f->ai[AI_IT] && before < f->ai[AI_IT] && f->ai_count >= f->ai[AI_IT]) {
    f->ai[AI_CR] |= AICR_AIINT;
    sync_ai_irq(f);
  }
}

static void sync_vi_irq(Flipper *f) {
  u16 di0 = f->vi[VI_DI0_OFF / 2];
  int status = (di0 & 0x8000u) != 0;
  int enable = (di0 & 0x1000u) != 0;
  if (status && enable)
    f->pi_cause |= PI_CAUSE_VI;
  else
    f->pi_cause &= ~PI_CAUSE_VI;
}

void flipper_advance(Flipper *f, u64 cycles) {
  f->cycles += cycles;
  ai_advance(f, cycles);
  f->line_acc += cycles;
  u64 lines = f->line_acc / VI_CYCLES_PER_LINE;
  f->line_acc -= lines * VI_CYCLES_PER_LINE;
  f->vcount += (u32)lines;
  f->hcount = (u32)((f->hcount + cycles) % 430u);
  while (f->vcount >= VI_NTSC_LINES) {
    f->vcount -= VI_NTSC_LINES;
    f->retrace++;
    f->vi[VI_DI0_OFF / 2] |= 0x8000u;
    sync_vi_irq(f);
  }
  if (f->ipc_ack_delay) {
    f->ipc_ack_delay--;
    if (f->ipc_ack_delay == 0)
      ipc_update_irq(f);
  }
}

/* The IPC interrupt is a level, not an edge: Starlet holds it asserted while a
 * reply the PPC has not collected is still sitting in the register. Modelling
 * it as a one-shot loses any reply posted while the guest is inside its own IPC
 * handler, because the handler clears the flag after acknowledging Y1. So the
 * flag is re-derived from the control bits wherever they or the masks move,
 * which is what the hardware does. */
void ipc_update_irq(Flipper *f) {
  u32 ctrl = f->ipc_ctrl;
  if (((ctrl & IPC_CTRL_Y1) && (ctrl & IPC_CTRL_IY1)) ||
      ((ctrl & IPC_CTRL_Y2) && (ctrl & IPC_CTRL_IY2)))
    f->ipc_irq_flag |= 0x40000000u;
  if (f->ipc_irq_flag & f->ipc_irq_mask)
    f->pi_cause |= PI_CAUSE_IPC;
  else
    f->pi_cause &= ~PI_CAUSE_IPC;
}

static u32 load_u16_file(const u16 *regs, u32 count, u32 off, u8 size) {
  u32 idx = off / 2;
  if (idx >= count)
    return 0;
  if (size <= 2)
    return regs[idx];
  u32 hi = regs[idx];
  u32 lo = (idx + 1 < count) ? regs[idx + 1] : 0;
  return (hi << 16) | lo;
}

static void store_u16_file(u16 *regs, u32 count, u32 off, u8 size, u64 value) {
  u32 idx = off / 2;
  if (idx >= count)
    return;
  if (size <= 2) {
    regs[idx] = (u16)value;
    return;
  }
  regs[idx] = (u16)(value >> 16);
  if (idx + 1 < count)
    regs[idx + 1] = (u16)value;
}

static u32 load_u32_file(const u32 *regs, u32 count, u32 off, u8 size) {
  u32 idx = off / 4;
  if (idx >= count)
    return 0;
  u32 v = regs[idx];
  if (size >= 4)
    return v;
  if (size == 2)
    return (off & 2u) ? (v & 0xFFFFu) : (v >> 16);
  u32 shift = 24u - 8u * (off & 3u);
  return (v >> shift) & 0xFFu;
}

static void store_u32_file(u32 *regs, u32 count, u32 off, u8 size, u64 value) {
  u32 idx = off / 4;
  if (idx >= count)
    return;
  if (size >= 4) {
    regs[idx] = (u32)value;
    return;
  }
  if (size == 2) {
    if (off & 2u)
      regs[idx] = (regs[idx] & 0xFFFF0000u) | (u16)value;
    else
      regs[idx] = (regs[idx] & 0x0000FFFFu) | ((u32)value << 16);
    return;
  }
  u32 shift = 24u - 8u * (off & 3u);
  u32 mask = 0xFFu << shift;
  regs[idx] = (regs[idx] & ~mask) | (((u32)value & 0xFFu) << shift);
}

static u64 read_flipper(Flipper *f, u32 off, u8 size) {
  if (off < 0x80u) {
    u32 v = load_u16_file(f->cp, 64, off, size);
    if (off == 0) {
      if (size <= 2)
        v |= 0x000C;
      else
        v |= 0x000C0000u;
    }
    return v;
  }
  if (off >= 0x1000u && off < 0x1020u)
    return load_u16_file(f->pe, 8, off - 0x1000u, size);
  if (off >= 0x2000u && off < 0x2080u) {
    u32 voff = off - 0x2000u;
    if (voff == VI_VCOUNT_OFF) {
      if (size <= 2)
        return f->vcount & 0x7FFu;
      return ((f->vcount & 0x7FFu) << 16) | (f->hcount & 0x7FFu);
    }
    if (voff == VI_HCOUNT_OFF)
      return f->hcount & 0x7FFu;
    return load_u16_file(f->vi, 64, voff, size);
  }
  if (off >= 0x3000u && off < 0x3040u) {
    switch (off - 0x3000u) {
    case 0x00:
      return f->pi_cause;
    case 0x04:
      return f->pi_mask;
    case 0x0C:
      return f->pi_fifo_base;
    case 0x10:
      return f->pi_fifo_end;
    case 0x14:
      return f->pi_fifo_wptr;
    case 0x2C:
      return 0x246500B1u;
    default:
      return 0;
    }
  }
  if (off >= 0x4000u && off < 0x4080u)
    return load_u16_file(f->mi, 64, off - 0x4000u, size);
  if (off >= 0x5000u && off < 0x5040u) {
    u32 doff = off - 0x5000u;
    if (doff == 0x04 || doff == 0x06) {
      /* DSPCheckMailFromDSP reads the high half and tests bit 15; reading the
       * low half (or the pair at once) is what retires the mail. */
      if (doff == 0x04 && size < 4)
        return dsp_latch_mail(f, 0) >> 16;
      u32 v = dsp_latch_mail(f, 1);
      f->dsp_last_mail = v & 0x7FFFFFFFu;
      return doff == 0x04 ? v : (v & 0xFFFFu);
    }
    if (doff == 0x0Au) {
      /* DSPInitCode reads back set exactly once: the SDK spins on it until the
       * DSP has taken the init ucode, and we take it immediately. */
      u16 csr = f->dsp[0x0A / 2];
      f->dsp[0x0A / 2] = (u16)(csr & ~DSP_CSR_INIT_CODE);
      return csr;
    }
    return load_u16_file(f->dsp, 32, doff, size);
  }
  if (off >= 0x6000u && off < 0x6030u)
    return load_u32_file(f->di, 12, off - 0x6000u, size);
  if (off >= 0x6400u && off < 0x6500u)
    return load_u32_file(f->si, 16, off - 0x6400u, size);
  if (off >= 0x6800u && off < 0x6840u)
    return load_u32_file(f->exi, 16, off - 0x6800u, size);
  if (off >= 0x6C00u && off < 0x6C10u) {
    /* AISCNT has no storage of its own; publish the free-running value so a
     * byte or halfword read of it is extracted the same way as any other. */
    f->ai[AI_SCNT] = f->ai_count;
    return load_u32_file(f->ai, 4, off - 0x6C00u, size);
  }
  return 0;
}

static void write_flipper(Flipper *f, CPUState *cpu, GxCp *gx, u32 off,
                          u64 value, u8 size) {
  if (off >= 0x8000u && off < 0x9000u) {
    if (gx) {
      gx_cp_write(gx, value, size);
      pe_drain(f, gx);
    }
    return;
  }
  if (off < 0x80u) {
    store_u16_file(f->cp, 64, off, size, value);
    f->cp[0] |= 0x000C;
    return;
  }
  if (off >= 0x1000u && off < 0x1020u) {
    u32 poff = off - 0x1000u;
    store_u16_file(f->pe, 8, poff, size, value);
    if (poff <= PE_CTRL_OFF && poff + size > PE_CTRL_OFF) {
      u16 want = f->pe[PE_CTRL_OFF / 2];
      if (want & PE_CTRL_TOKEN_ACK)
        f->pe_token_irq = 0;
      if (want & PE_CTRL_FINISH_ACK)
        f->pe_finish_irq = 0;
      f->pe[PE_CTRL_OFF / 2] =
          (u16)(want & (PE_CTRL_TOKEN_ENABLE | PE_CTRL_FINISH_ENABLE));
      pe_update_irq(f);
    }
    return;
  }
  if (off >= 0x2000u && off < 0x2080u) {
    u32 voff = off - 0x2000u;
    store_u16_file(f->vi, 64, voff, size, value);
    if (voff <= VI_DI0_OFF && voff + size > VI_DI0_OFF)
      sync_vi_irq(f);
    return;
  }
  if (off >= 0x3000u && off < 0x3040u) {
    switch (off - 0x3000u) {
    case 0x00:
      f->pi_cause &= ~(u32)value;
      f->pi_cause |= PI_CAUSE_RST;
      if ((u32)value & PI_CAUSE_IPC) {
        f->ipc_irq_flag &= ~0x40000000u;
        ipc_update_irq(f);
      }
      break;
    case 0x04:
      f->pi_mask = (u32)value;
      break;
    case 0x0C:
      f->pi_fifo_base = (u32)value;
      break;
    case 0x10:
      f->pi_fifo_end = (u32)value;
      break;
    case 0x14:
      f->pi_fifo_wptr = (u32)value;
      break;
    default:
      break;
    }
    return;
  }
  if (off >= 0x4000u && off < 0x4080u) {
    store_u16_file(f->mi, 64, off - 0x4000u, size, value);
    return;
  }
  if (off >= 0x5000u && off < 0x5040u) {
    u32 doff = off - 0x5000u;
    if (doff == 0x0Au) {
      dsp_write_csr(f, (u16)value);
      return;
    }
    store_u16_file(f->dsp, 32, doff, size, value);
    /* CPU->DSP mailbox: the SDK writes the high half then the low half, and the
     * low write is what hands the mail over. Our DSP consumes it immediately,
     * so clear the "mail pending" bit the high half reports back --
     * DSPCheckMailToDSP spins on it. */
    if (doff <= 0x02u && doff + size > 0x02u) {
      u32 mail = ((u32)f->dsp[0] << 16) | f->dsp[1];
      f->dsp[0] &= 0x7FFFu;
      dsp_handle_mail(f, mail);
    }
    /* Writing the low half of the ARAM DMA count is what arms the engine. */
    if (doff <= 0x2Au && doff + size > 0x2Au)
      aram_dma(f, cpu);
    return;
  }
  if (off >= 0x6000u && off < 0x6030u) {
    store_u32_file(f->di, 12, off - 0x6000u, size, value);
    if (off - 0x6000u == 0x1Cu)
      f->di[0] |= 0x10u;
    return;
  }
  if (off >= 0x6400u && off < 0x6500u) {
    store_u32_file(f->si, 16, off - 0x6400u, size, value);
    return;
  }
  if (off >= 0x6800u && off < 0x6840u) {
    u32 eoff = off - 0x6800u;
    store_u32_file(f->exi, 16, eoff, size, value);
    /* EXI CR bit 0 is TSTART; hardware clears it when the transfer ends. */
    if ((eoff == 0x0Cu || eoff == 0x20u || eoff == 0x34u) && (value & 1u)) {
      u32 idx = eoff / 4;
      f->exi[idx] &= ~1u;
    }
    return;
  }
  if (off >= 0x6C00u && off < 0x6C10u) {
    u32 aoff = off - 0x6C00u;
    u32 before = f->ai[AI_CR];
    store_u32_file(f->ai, 4, aoff, size, value);
    if (aoff >= 4u)
      return;
    u32 cr = f->ai[AI_CR];
    if (cr & AICR_SCRESET) {
      f->ai_count = 0;
      f->ai_acc = 0;
      cr &= ~AICR_SCRESET;
    }
    /* Writing one to AIINT acknowledges it; writing zero leaves it latched. */
    if (cr & AICR_AIINT)
      cr &= ~AICR_AIINT;
    else
      cr |= before & AICR_AIINT;
    /* Starting the counter or changing its rate restarts the fraction, so a
     * partial sample carried over from the old rate cannot skew the first
     * tick -- which is exactly the tick AIInit is timing. */
    if ((cr ^ before) & (AICR_PSTAT | AICR_AFR))
      f->ai_acc = 0;
    f->ai[AI_CR] = cr;
    sync_ai_irq(f);
  }
}

static u64 read_hollywood(Flipper *f, u32 off, u8 size) {
  /* Wii maps Flipper DI/SI/EXI/AI through the Hollywood window too. */
  if (off >= 0x6000u && off < 0x8000u)
    return read_flipper(f, off, size);
  switch (off) {
  case 0x00:
    return f->ipc_ppcmsg;
  case 0x04:
    return f->ipc_ctrl & 0x3Fu;
  case 0x08:
    return f->ipc_armmsg;
  case 0x30:
    return f->ipc_irq_flag;
  case 0x34:
    return f->ipc_irq_mask;
  case 0xC0:
  case 0xE0:
    return f->gpio_out;
  case 0xC4:
  case 0xE4:
    return f->gpio_dir;
  case 0xC8:
  case 0xE8:
    return 0x80u; /* SLOT_IN: disc present */
  case 0x64:
    return 0xFFFFFFFFu; /* AHBPROT: Broadway may access all */
  case 0x194:
    return f->hw_resets;
  default:
    return 0;
  }
}

static void write_hollywood(Flipper *f, CPUState *cpu, u32 off, u64 value,
                            u8 size) {
  if (off >= 0x6000u && off < 0x8000u) {
    write_flipper(f, cpu, NULL, off, value, size);
    return;
  }
  switch (off) {
  case 0x00:
    f->ipc_ppcmsg = (u32)value;
    break;
  case 0x04: {
    u32 v = (u32)value;
    int x1 = (v & 1u) != 0;
    if (v & 0x4u)
      f->ipc_ctrl &= ~0x4u; /* PPC clears Y1 */
    if (v & 0x2u)
      f->ipc_ctrl &= ~0x2u; /* PPC clears Y2 */
    f->ipc_ctrl = (f->ipc_ctrl & ~0x39u) | (v & 0x38u);
    /* Dolphin InitIPC GenerateAck(0): Starlet ACKs once the PPC enables IY.
     * Do this only after IY is set so OSInterruptInit cannot take an
     * unhandled PI IPC before IPCCltInit installs the handler. */
    if (!f->ipc_ready && (v & 0x30u)) {
      f->ipc_ready = 1;
      f->ipc_ctrl |= 0x2u; /* Y2 */
      /* Delay the ACK IRQ so IPCClt can OSSleepThread first (lost-wakeup). */
      f->ipc_ack_delay = 64;
    }
    if (x1 && f->ios)
      ios_doorbell(f->ios, f, cpu, f->ipc_ppcmsg);
    else if (x1) {
      f->ipc_x1 = 1;
      f->ipc_ctrl |= 1u;
    }
    /* Clearing Y1 frees the reply register, so anything IOS has waiting --
     * a Bluetooth read completed out of band, say -- can go out now. */
    if ((v & IPC_CTRL_Y1) && f->ios)
      ios_pump(f->ios, f, cpu);
    ipc_update_irq(f);
    break;
  }
  case 0x30:
    f->ipc_irq_flag &= ~(u32)value;
    ipc_update_irq(f);
    break;
  case 0x34:
    f->ipc_irq_mask = (u32)value;
    ipc_update_irq(f);
    break;
  case 0xC0:
  case 0xE0:
    f->gpio_out = (u32)value;
    break;
  case 0xC4:
  case 0xE4:
    f->gpio_dir = (u32)value;
    break;
  case 0x194:
    f->hw_resets = (u32)value;
    break;
  default:
    break;
  }
}

u64 flipper_read(Flipper *f, u32 ea, u8 size) {
  u32 off = 0;
  int hw = 0;
  note_ea(f, ea);
  if (!classify(ea, &off, &hw))
    return 0;
  if (hw)
    return read_hollywood(f, off, size);
  return read_flipper(f, off, size);
}

void flipper_write(Flipper *f, CPUState *cpu, GxCp *gx, u32 ea, u64 value,
                   u8 size) {
  u32 off = 0;
  int hw = 0;
  note_ea(f, ea);
  if (!classify(ea, &off, &hw))
    return;
  if (hw)
    write_hollywood(f, cpu, off, value, size);
  else
    write_flipper(f, cpu, gx, off, value, size);
}
