#include "chassis.h"
#include "hle_hooks.h"
#include "frontend/container/dol.h"
#include "generated.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#define SLICE_CYCLES 10000
/* 729 MHz divided by the Wii Remote's 200 Hz reporting rate. */
#define CPU_CYCLES_PER_WIIMOTE_SAMPLE 3645000u
/* Broadway core clock over timebase clock: 729 MHz / 60.75 MHz. */
#define CPU_CYCLES_PER_TB 12u
#define SPIN_SAME 2048u

#define LOMEM_BOOT_MAGIC 0x80000020u
#define LOMEM_BOOT_VERSION 0x80000024u
#define LOMEM_PHYS_MEM 0x80000028u
#define LOMEM_CONSOLE_TYPE 0x8000002Cu
#define LOMEM_ARENA_LO 0x80000030u
#define LOMEM_ARENA_HI 0x80000034u
#define LOMEM_VIDEO_MODE 0x800000CCu
#define LOMEM_SIM_MEM 0x800000F0u
#define LOMEM_BUS_CLOCK 0x800000F8u
#define LOMEM_CPU_CLOCK 0x800000FCu

#define BROADWAY_PVR 0x00087102u
#define WII_BUS_CLOCK 0x0E7BE2C0u
#define WII_CPU_CLOCK 0x2B73A840u
#define WII_ARENA_HI 0x817FEC60u
#define WII_CONSOLE_TYPE 0x00000023u

#define PPC_HALT_LO 0x804A096Cu
#define PPC_HALT_HI 0x804A097Cu
#define OS_REPORT_PC 0x804A3E94u
#define OS_PANIC_HANDLER 0x804B13D4u
#define OS_PANIC_ENTRY 0x804B141Cu
#define MSR_EE 0x00008000u
/* The MSR bits rfi copies back out of SRR1. */
#define MSR_RFI_MASK 0x87C0FFFFu
#define PI_IRQ_VECTOR 0x80000500u
#define DEC_VECTOR 0x80000900u
#define OS_IDLE_WAIT 0x804AB358u
/* The displacement in the idle wait's own lwz: r13 is the small-data base and
 * the word is the thread run queue's head. Read by the idle-skip fold and by
 * the one-off idle log below, which is where it was first worked out. */
#define OS_IDLE_RUNQ_OFF (-8048)
/* How often a folded idle dispatch is run for real instead, to re-derive what
 * a slice costs. 1/256 of the idle time, against getting the cycle count
 * silently wrong forever. */
#define IDLE_RECHECK 256u
/* The Bluetooth stack's own trace sink. WUD embeds a Broadcom BTE stack whose
 * BTU/BTM/L2CAP layers all funnel their diagnostics through this one function
 * as (mask, format, ...), so catching it here is how the stack says what it is
 * doing -- there is no other way to see inside it. */
#define BTE_TRACE_PC 0x804EC958u
#define OS_INIT_IPC 0x804BFD58u
#define OS_INIT_PC 0x804A155Cu
#define MAIN_PC 0x803994D0u
#define OS_SLEEP_THREAD 0x804ABFBCu
#define OS_FATAL 0x804A6C1Cu
#define OS_PANIC 0x804A3F24u
#define DVD_CONVERT_PATH 0x804BEF5Cu
#define IOS_OPEN_SYNC 0x804D27E8u
#define IOS_OPEN_ASYNC 0x804D26D0u

static const u32 k_smc_sites[] = {
    0x80004320u, 0x800046C8u, 0x8015D2F0u, 0x80442AD4u, 0x80442AE0u,
    0x80442AE8u, 0x804A1ACCu, 0x804A1B20u, 0x804A1B54u, 0x804A1C30u,
    0x804A3020u, 0x804C3C30u, 0x804CC594u, 0x804CC5D0u, 0x804CC5E4u,
    0x804CD534u, 0x804CD544u, 0x804CE3D0u, 0x80529968u,
};

/* Guest functions answered natively instead of being recompiled through.
 * The Vita has no Bluetooth radio and no Wii Remote, so everything below WPAD
 * -- WUD, HCI, L2CAP -- is dead weight in this port: emulating a dongle just to
 * satisfy it would be a lot of machinery whose only output is synthetic input
 * we intend to generate ourselves. Instead the boot path's WUD entry points are
 * answered here and the stack stays dormant.
 *
 * A hook runs with the guest's argument registers live and returns through lr,
 * exactly like a `blr` would. Every recompiled `bl` returns to this dispatch
 * loop, so matching on the entry PC catches every call site. */
typedef void (*HleFn)(Chassis *ch);

/* Defined below, next to the other logging helpers. */
static void ch_logf(Chassis *ch, const char *fmt, ...);

/* WUDIsLinkedWBC() -- WUDInit busy-waits on this until the Bluetooth state
 * machine reaches state 5. With no dongle the state byte at 0x80692846 never
 * leaves 0, so the wait never ends; report the stack as up.
 *
 * That was written when there was no Bluetooth stack at all. There is one now,
 * and it completes a full HCI bring-up, so the state machine may well reach 5
 * on its own -- in which case this hook is not merely redundant but harmful,
 * because answering "linked" early skips whatever WUDInit does after the wait,
 * and WPAD registration hangs off that. GV_WUD_LOG=n prints what the real
 * state byte says on the first n calls, which settles it without changing
 * behaviour; GV_WUD_REAL=1 then answers from the byte instead of lying. */
static void hle_wud_is_ready(Chassis *ch) {
  u8 state = mem_read8(&ch->cpu, 0x80692846u);
  if (ch->wud_log) {
    ch->wud_log--;
    ch_logf(ch, "wud is_linked: state=0x%02x at hits=%u", state,
            ch->dispatch_hits);
  }
  ch->cpu.gpr[3] = ch->wud_real ? (state == 5u) : 1u;
}

/* The Home Button menu. SMG keeps it out of main.dol: the function at
 * 0x803ACA54 loads /ModuleData/HomeButtonMenuWrapperRSO.rso into MEM2, runs the
 * module's prolog, and resolves seven HBM*RSO exports into globals that the
 * thunks at 0x803ACB88..0x803ACBD0 jump through. That module is the only
 * dynamically loaded code in the game, and recompiling it ahead of time is not
 * possible: it is relocated to wherever the MEM2 allocator puts it. It is also
 * Wii system UI with no counterpart on a Vita, so the whole wrapper is answered
 * here -- loader included, so no RSO code is ever entered.
 *
 * Every export returns void except HBMGetSelectBtnNumRSO. Its caller at
 * 0x803625D4 reads -1 as "the menu is still up" and 1 as "resume the game", so
 * a menu that is never drawn has to answer 1 or the game would hang waiting for
 * a button nobody can press. */
static void hle_hbm_load_module(Chassis *ch) { (void)ch; }
static void hle_hbm_void(Chassis *ch) { (void)ch; }
static void hle_hbm_select_btn(Chassis *ch) { ch->cpu.gpr[3] = 1u; }

#define HLE_ENTRY(addr, fn, name) {addr, fn, name},
static const struct {
  u32 pc;
  HleFn fn;
  const char *name;
} k_hle[] = {GALAXYVITA_HLE_HOOKS(HLE_ENTRY)};
#undef HLE_ENTRY

static int hle_call(Chassis *ch, u32 pc) {
  for (u32 i = 0; i < sizeof(k_hle) / sizeof(k_hle[0]); i++) {
    if (k_hle[i].pc != pc)
      continue;
    k_hle[i].fn(ch);
    ch->cpu.pc = ch->cpu.lr;
    ch->hle_calls++;
    return 1;
  }
  return 0;
}

static void hot_note(Chassis *ch, u32 pc) {
  u32 h = (pc >> 2) * 2654435761u;
  for (u32 i = 0; i < 8u; i++) {
    ChassisHotSlot *const slot = &ch->hot[(h + i) & (CHASSIS_HOT_SLOTS - 1u)];
    if (slot->hits == 0u || slot->pc == pc) {
      slot->pc = pc;
      slot->hits++;
      return;
    }
  }
}

u32 chassis_top_pcs(const Chassis *ch, u32 *out_pc, u32 *out_hits, u32 n) {
  u32 got = 0;
  for (u32 s = 0; s < CHASSIS_HOT_SLOTS; s++) {
    u32 hits = ch->hot[s].hits;
    if (!hits)
      continue;
    u32 pos = got;
    while (pos > 0 && out_hits[pos - 1u] < hits)
      pos--;
    if (pos >= n)
      continue;
    u32 last = got < n ? got : n - 1u;
    for (u32 j = last; j > pos; j--) {
      out_pc[j] = out_pc[j - 1u];
      out_hits[j] = out_hits[j - 1u];
    }
    out_pc[pos] = ch->hot[s].pc;
    out_hits[pos] = hits;
    if (got < n)
      got++;
  }
  return got;
}

static void ch_log(Chassis *ch, const char *msg) {
  if (ch->log)
    ch->log(msg, ch->log_user);
  else
    fprintf(stderr, "%s\n", msg);
}

static void ch_logf(Chassis *ch, const char *fmt, ...) {
  char buf[192];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(buf, sizeof(buf), fmt, ap);
  va_end(ap);
  ch_log(ch, buf);
}

static void poke32(CPUState *cpu, u32 addr, u32 value) {
  mem_write32(cpu, addr, value);
}

static void poke16(CPUState *cpu, u32 addr, u16 value) {
  mem_write16(cpu, addr, value);
}

static void guest_str(CPUState *cpu, u32 ea, char *out, u32 cap) {
  u32 i = 0;
  if (!ea || cap == 0) {
    if (cap)
      out[0] = 0;
    return;
  }
  for (; i + 1 < cap; i++) {
    u8 c = mem_read8(cpu, ea + i);
    if (c < 32 || c > 126) {
      if (c == 0)
        break;
      out[i] = '.';
    } else
      out[i] = (char)c;
    if (!c)
      break;
  }
  out[i] = 0;
}

static int is_ppchalt(u32 pc) {
  return pc >= PPC_HALT_LO && pc <= PPC_HALT_HI;
}

static int phys_host(CPUState *cpu, u32 ea, u8 **host, u32 *avail) {
  if (ea < cpu->ram_size) {
    *host = cpu->ram + ea;
    *avail = cpu->ram_size - ea;
    return 1;
  }
  if (cpu->mem2 && ea >= 0x10000000u &&
      ea < 0x10000000u + cpu->mem2_size) {
    u32 off = ea - 0x10000000u;
    *host = cpu->mem2 + off;
    *avail = cpu->mem2_size - off;
    return 1;
  }
  return 0;
}

/* Display-list targets arrive as physical addresses (GXCallDisplayList masks
 * the pointer with 0x3FFFFFFF). Handing the parser a direct pointer is safe
 * because a list is executed synchronously, before the CPU runs again. */

static u64 phys_load(const u8 *p, u8 size) {
  u64 v = 0;
  for (u8 i = 0; i < size; i++)
    v = (v << 8) | p[i];
  return v;
}

static void phys_store(u8 *p, u64 value, u8 size) {
  for (u8 i = 0; i < size; i++)
    p[i] = (u8)(value >> ((size - 1u - i) * 8u));
}

static u64 ext_read(CPUState *cpu, u32 ea, u8 size) {
  Chassis *ch = (Chassis *)cpu->external_user_data;
  u8 *host;
  u32 avail;
  if (phys_host(cpu, ea, &host, &avail) && avail >= size)
    return phys_load(host, size);
  ch->mmio_reads++;
  if ((ea & 0xFFFFFF00u) == 0xCD000000u && ch->dispatch_hits < 64u)
    ch_logf(ch, "hwipc r ea=0x%08x hits=%u pc=0x%08x", ea, ch->dispatch_hits,
            cpu->pc);
  return flipper_read(&ch->flipper, ea, size);
}

/* The write-gather pipe, at offset 0x8000..0x8FFF of the Flipper window --
 * both the 0xCC cached view and the 0x0C physical alias, because flipper.c's
 * classify() takes either and the guest uses both. */
static int is_wgp(u32 ea) {
  u32 top = ea >> 24;
  u32 off = ea & 0x00FFFFFFu;
  return (top == 0xCCu || top == 0x0Cu) && off >= 0x8000u && off < 0x9000u;
}

static void ext_write(CPUState *cpu, u32 ea, u64 value, u8 size) {
  Chassis *ch = (Chassis *)cpu->external_user_data;
  u8 *host;
  u32 avail;
  if (phys_host(cpu, ea, &host, &avail) && avail >= size) {
    phys_store(host, value, size);
    return;
  }
  ch->mmio_writes++;
  /* One answer, used twice. `in_fifo` and `is_wgp` were the same four compares
   * written out separately, and this is 99.9% of every MMIO store in the
   * game. */
  const int wgp = is_wgp(ea);
  if (wgp) {
    ch->fifo_writes++;
    if (!ch->fifo_hit) {
      ch->fifo_hit = 1;
      ch->fifo_ea = ea;
      ch->fifo_pc = cpu->pc;
      ch_logf(ch, "GX FIFO write ea=0x%08x pc=0x%08x hits=%u", ea, cpu->pc,
              ch->dispatch_hits);
    }
  }
  if (((ea & ~3u) == 0xCD000004u || (ea & ~3u) == 0x0D000004u) &&
      ch->ios.log_left) {
    ch->ios.log_left--;
    ch_logf(ch, "ppcctrl w val=0x%08x pc=0x%08x hits=%u", (u32)value, cpu->pc,
            ch->dispatch_hits);
  }
  /* The GX FIFO window is the write-gather pipe, and everything under it --
   * command decode, the vertex loader, the GXM sink -- runs on this thread
   * inside this call. Splitting it out here is what tells a CPU-bound run from
   * a draw-submission-bound one, which no device measurement has ever done. */
  {
    u8 was = ch->phase;
    ch->phase = wgp ? CHASSIS_PH_GX : CHASSIS_PH_MMIO;
    flipper_write(&ch->flipper, cpu, ch->gx, ea, value, size);
    ch->phase = was;
  }
}

static u32 ext_read32(CPUState *cpu, u32 ea, u8 rid) {
  (void)rid;
  return (u32)ext_read(cpu, ea, 4);
}

static void ext_write32(CPUState *cpu, u32 ea, u32 value, u8 rid) {
  (void)rid;
  ext_write(cpu, ea, value, 4);
}

static u32 spr_read(CPUState *cpu, u16 spr, u32 cia) {
  Chassis *ch = (Chassis *)cpu->external_user_data;
  (void)cia;
  if (spr == 287)
    return BROADWAY_PVR;
  if (spr == 22)
    return ch->dec;
  if (spr == 268)
    return (u32)cpu->timebase;
  if (spr == 269)
    return (u32)(cpu->timebase >> 32);
  if (spr >= 272 && spr <= 275)
    return ch->sprg[spr - 272];
  return 0;
}

static void spr_write(CPUState *cpu, u16 spr, u32 value, u32 cia) {
  Chassis *ch = (Chassis *)cpu->external_user_data;
  (void)cia;
  if (spr == 22) {
    ch->dec_writes++;
    ch->dec = value;
    ch->dec_irq = 0;
    return;
  }
  if (spr == 284) {
    cpu->timebase = (cpu->timebase & 0xFFFFFFFF00000000ull) | value;
    return;
  }
  if (spr == 285)
    cpu->timebase = ((u64)value << 32) | (cpu->timebase & 0xFFFFFFFFull);
  if (spr >= 272 && spr <= 275)
    ch->sprg[spr - 272] = value;
}

/* Broadway SPRs the recompiler's access table does not carry. HID4 is the one
 * that matters: __OSPSInit writes it while bringing the paired singles up, and
 * without a home here ppc_mtspr calls it illegal. */
static int extra_spr(Chassis *ch, u16 spr, u32 *value, int write) {
  if (spr != 1011u) /* HID4 */
    return 0;
  if (write)
    ch->hid4 = *value;
  else
    *value = ch->hid4;
  return 1;
}

static void fallback(CPUState *cpu, u32 raw, u32 cia) {
  Chassis *ch = (Chassis *)cpu->external_user_data;
  u32 opcd = raw >> 26;
  u32 xo = (raw >> 1) & 0x3FFu;
  u8 rd = (u8)((raw >> 21) & 31u);
  /* The instruction's 10-bit SPR field carries the number's two halves the
   * other way round: bits 11-15 hold the low five, bits 16-20 the high five.
   * Reading it straight turns mtdbat2u (540) into mtgqr0 (912), which quietly
   * repoints every paired-single store at a quantised format. */
  u16 spr = (u16)((((raw >> 11) & 31u) << 5) | ((raw >> 16) & 31u));

  if (opcd == 31u && (xo == 339u || xo == 467u || xo == 371u)) {
    /* An SPR the helpers do not know raises a program exception, and taking
     * one costs MSR[EE], IR, DR and -- the expensive bit -- RI. Clearing
     * cpu->exception afterwards hides the trap but not the damage: with RI
     * gone the SDK's exception stub reads every later interrupt as
     * unrecoverable and sends it to __OSUnhandledException instead of the
     * handler table, which kills the decrementer and with it OSAlarm. So the
     * whole machine state is rolled back, not just the exception flag. */
    u32 msr = cpu->msr, srr0 = cpu->srr0, srr1 = cpu->srr1;
    u32 exc = cpu->exception;
    u32 v = cpu->gpr[rd];
    int handled = 1;
    cpu->exception = 0;
    if (xo == 339u)
      ch->fb_mfspr++;
    else if (xo == 467u)
      ch->fb_mtspr++;
    if (extra_spr(ch, spr, &v, xo == 467u)) {
      if (xo != 467u)
        cpu->gpr[rd] = v;
    } else if (xo == 339u) {
      v = ppc_mfspr(cpu, spr, cia);
      handled = cpu->exception == 0u;
      if (handled)
        cpu->gpr[rd] = v;
    } else if (xo == 467u) {
      ppc_mtspr(cpu, spr, v, cia);
      handled = cpu->exception == 0u;
    } else {
      ch->fb_mftb++;
      v = ppc_mftb(cpu, spr, cia);
      handled = cpu->exception == 0u;
      if (handled)
        cpu->gpr[rd] = v;
    }
    if (!handled) {
      cpu->msr = msr;
      cpu->srr0 = srr0;
      cpu->srr1 = srr1;
      if (ch->spr_skips++ < 8u)
        ch_logf(ch, "skip spr=%u raw=0x%08x cia=0x%08x", spr, raw, cia);
    }
    cpu->exception = exc;
    cpu->pc = cia + 4u;
    return;
  }

  ch->fb_unknown++;
  if (ch->dispatch_hits < 32u)
    ch_logf(ch, "skip unknown raw=0x%08x cia=0x%08x xo=%u", raw, cia, xo);
  cpu->pc = cia + 4u;
}

static int map_bytes(Chassis *ch, u32 addr, const u8 *src, u32 size) {
  CPUState *cpu = &ch->cpu;
  if (size == 0)
    return 1;
  if (addr >= GC_RAM_BASE && addr + size <= GC_RAM_BASE + cpu->ram_size) {
    memcpy(cpu->ram + (addr - GC_RAM_BASE), src, size);
    return 1;
  }
  if (cpu->mem2 && addr >= WII_MEM2_BASE &&
      addr + size <= WII_MEM2_BASE + cpu->mem2_size) {
    memcpy(cpu->mem2 + (addr - WII_MEM2_BASE), src, size);
    return 1;
  }
  ch_logf(ch, "section 0x%08x size 0x%08x does not fit guest RAM (extra=0x%08x)",
          addr, size, 0);
  return 0;
}

static int zero_guest(Chassis *ch, u32 addr, u32 size) {
  CPUState *cpu = &ch->cpu;
  if (size == 0)
    return 1;
  if (addr >= GC_RAM_BASE && addr + size <= GC_RAM_BASE + cpu->ram_size) {
    memset(cpu->ram + (addr - GC_RAM_BASE), 0, size);
    return 1;
  }
  if (cpu->mem2 && addr >= WII_MEM2_BASE &&
      addr + size <= WII_MEM2_BASE + cpu->mem2_size) {
    memset(cpu->mem2 + (addr - WII_MEM2_BASE), 0, size);
    return 1;
  }
  return 0;
}

static int in_vector_page(u32 pc) {
  u32 a = pc & 0x3FFFFFFFu;
  return a >= 0x100u && a < 0x1000u;
}

static u32 rlwinm_mask(u32 mb, u32 me) {
  u32 begin = 0xFFFFFFFFu >> mb;
  u32 end = 0xFFFFFFFFu << (31u - me);
  if (mb <= me)
    return begin & end;
  return begin | end;
}

/* Record a dot-form result in CR0, the way the hardware does: LT/GT/EQ from
 * the signed result, SO copied from XER. The exception vectors branch on these
 * -- `rlwinm. r5, r5, 0, 30, 30` followed by `bne` is how the SDK's stub asks
 * whether MSR[RI] was set -- so dropping the update sends every exception down
 * whichever arm the stale CR0 happens to name. */
static void set_cr0(CPUState *cpu, u32 result) {
  u32 c = (s32)result < 0 ? 8u : (result == 0u ? 2u : 4u);
  if (cpu->xer & 0x80000000u)
    c |= 1u;
  cpu->cr = (cpu->cr & 0x0FFFFFFFu) | (c << 28);
}

static int interp_one(Chassis *ch, u32 w, u32 *pc) {
  CPUState *cpu = &ch->cpu;
  u32 op = w >> 26;
  u32 rd = (w >> 21) & 31u;
  u32 ra = (w >> 16) & 31u;
  u32 rb = (w >> 11) & 31u;
  u32 simm = w & 0xFFFFu;
  s32 simms = (s32)(s16)simm;
  u32 cia = *pc;

  if (w == 0x4C000064u) {
    *pc = cpu->srr0;
    cpu->msr = cpu->srr1;
    return 2;
  }
  if (op == 18u) {
    u32 li = w & 0x03FFFFFCu;
    if (li & 0x02000000u)
      li -= 0x04000000u;
    u32 dest = ((w >> 1) & 1u) ? li : cia + li;
    if (w & 1u)
      cpu->lr = cia + 4u;
    *pc = dest;
    return 1;
  }
  if (op == 19u) {
    u32 xo = (w >> 1) & 0x3FFu;
    if (xo == 16u) {
      *pc = cpu->lr;
      return 1;
    }
    if (xo == 528u) {
      *pc = cpu->ctr;
      return 1;
    }
    if (xo == 150u || xo == 0u) {
      *pc = cia + 4u;
      return 1;
    }
    return 0;
  }
  if (op == 31u) {
    u32 xo = (w >> 1) & 0x3FFu;
    if (xo == 83u) {
      cpu->gpr[rd] = cpu->msr;
      *pc = cia + 4u;
      return 1;
    }
    if (xo == 146u) {
      cpu->msr = cpu->gpr[rd];
      *pc = cia + 4u;
      return 1;
    }
    if (xo == 339u || xo == 467u) {
      /* Architectural SPR number: the instruction SPR field is 5-bit swapped. */
      u16 spr = (u16)((rb << 5) | ra);
      if (xo == 339u) {
        if (spr == 1)
          cpu->gpr[rd] = cpu->xer;
        else if (spr == 8)
          cpu->gpr[rd] = cpu->lr;
        else if (spr == 9)
          cpu->gpr[rd] = cpu->ctr;
        else if (spr == 26)
          cpu->gpr[rd] = cpu->srr0;
        else if (spr == 27)
          cpu->gpr[rd] = cpu->srr1;
        else if (spr >= 272 && spr <= 275)
          cpu->gpr[rd] = ch->sprg[spr - 272];
        else
          cpu->gpr[rd] = 0;
      } else {
        if (spr == 1)
          cpu->xer = cpu->gpr[rd];
        else if (spr == 8)
          cpu->lr = cpu->gpr[rd];
        else if (spr == 9)
          cpu->ctr = cpu->gpr[rd];
        else if (spr == 26)
          cpu->srr0 = cpu->gpr[rd];
        else if (spr == 27)
          cpu->srr1 = cpu->gpr[rd];
        else if (spr >= 272 && spr <= 275)
          ch->sprg[spr - 272] = cpu->gpr[rd];
      }
      *pc = cia + 4u;
      return 1;
    }
    if (xo == 19u) { /* mfcr */
      cpu->gpr[rd] = cpu->cr;
      *pc = cia + 4u;
      return 1;
    }
    if (xo == 144u) { /* mtcrf */
      u32 fxm = (w >> 12) & 0xFFu;
      u32 mask = 0;
      for (u32 i = 0; i < 8; i++)
        if (fxm & (1u << (7u - i)))
          mask |= 0xFu << (28u - i * 4u);
      cpu->cr = (cpu->cr & ~mask) | (cpu->gpr[rd] & mask);
      *pc = cia + 4u;
      return 1;
    }
    if (xo == 0u || xo == 32u) {
      u32 crfd = rd >> 2;
      u32 a = ra ? cpu->gpr[ra] : 0u;
      u32 b = cpu->gpr[rb];
      u32 c = 0;
      if (xo == 0u) {
        s32 sa = (s32)a, sb = (s32)b;
        if (sa < sb)
          c = 8;
        else if (sa > sb)
          c = 4;
        else
          c = 2;
      } else {
        if (a < b)
          c = 8;
        else if (a > b)
          c = 4;
        else
          c = 2;
      }
      cpu->cr = (cpu->cr & ~(0xFu << (28 - crfd * 4))) | (c << (28 - crfd * 4));
      *pc = cia + 4u;
      return 1;
    }
    if (xo == 444u || xo == 28u || xo == 316u || xo == 476u) {
      u32 a = cpu->gpr[rd], b = cpu->gpr[rb];
      u32 v = xo == 444u   ? (a | b)
              : xo == 28u  ? (a & b)
              : xo == 316u ? (a ^ b)
                           : ~(a & b);
      cpu->gpr[ra] = v;
      if (w & 1u)
        set_cr0(cpu, v);
      *pc = cia + 4u;
      return 1;
    }
    if (xo == 24u || xo == 536u) {
      u32 sh = cpu->gpr[rb] & 63u;
      u32 v = sh >= 32u  ? 0u
              : xo == 24u ? (cpu->gpr[rd] << sh)
                          : (cpu->gpr[rd] >> sh);
      cpu->gpr[ra] = v;
      if (w & 1u)
        set_cr0(cpu, v);
      *pc = cia + 4u;
      return 1;
    }
    if (xo == 598u || xo == 854u) {
      *pc = cia + 4u;
      return 1;
    }
    return 0;
  }
  if (op == 14u) {
    cpu->gpr[rd] = (ra ? cpu->gpr[ra] : 0u) + (u32)simms;
    *pc = cia + 4u;
    return 1;
  }
  if (op == 15u) {
    cpu->gpr[rd] = (ra ? cpu->gpr[ra] : 0u) + (simm << 16);
    *pc = cia + 4u;
    return 1;
  }
  if (op == 24u) {
    cpu->gpr[ra] = cpu->gpr[rd] | simm;
    *pc = cia + 4u;
    return 1;
  }
  if (op == 25u) {
    cpu->gpr[ra] = cpu->gpr[rd] | (simm << 16);
    *pc = cia + 4u;
    return 1;
  }
  if (op == 21u) {
    u32 sh = rb, mb = (w >> 6) & 31u, me = (w >> 1) & 31u;
    u32 v = cpu->gpr[rd];
    if (sh)
      v = (v << sh) | (v >> (32u - sh));
    v &= rlwinm_mask(mb, me);
    cpu->gpr[ra] = v;
    if (w & 1u)
      set_cr0(cpu, v);
    *pc = cia + 4u;
    return 1;
  }
  if (op == 28u || op == 29u) { /* andi. / andis. -- always dot-form */
    u32 v = cpu->gpr[rd] & (op == 28u ? simm : (simm << 16));
    cpu->gpr[ra] = v;
    set_cr0(cpu, v);
    *pc = cia + 4u;
    return 1;
  }
  if (op == 32u) {
    u32 ea = (ra ? cpu->gpr[ra] : 0u) + (u32)simms;
    cpu->gpr[rd] = mem_read32(cpu, ea);
    *pc = cia + 4u;
    return 1;
  }
  if (op == 36u) {
    u32 ea = (ra ? cpu->gpr[ra] : 0u) + (u32)simms;
    mem_write32(cpu, ea, cpu->gpr[rd]);
    *pc = cia + 4u;
    return 1;
  }
  if (op == 34u) {
    u32 ea = (ra ? cpu->gpr[ra] : 0u) + (u32)simms;
    cpu->gpr[rd] = mem_read8(cpu, ea);
    *pc = cia + 4u;
    return 1;
  }
  if (op == 38u) {
    u32 ea = (ra ? cpu->gpr[ra] : 0u) + (u32)simms;
    mem_write8(cpu, ea, (u8)cpu->gpr[rd]);
    *pc = cia + 4u;
    return 1;
  }
  if (op == 40u) {
    u32 ea = (ra ? cpu->gpr[ra] : 0u) + (u32)simms;
    cpu->gpr[rd] = mem_read16(cpu, ea);
    *pc = cia + 4u;
    return 1;
  }
  if (op == 44u) {
    u32 ea = (ra ? cpu->gpr[ra] : 0u) + (u32)simms;
    mem_write16(cpu, ea, (u16)cpu->gpr[rd]);
    *pc = cia + 4u;
    return 1;
  }
  if (op == 42u) {
    u32 ea = (ra ? cpu->gpr[ra] : 0u) + (u32)simms;
    cpu->gpr[rd] = (u32)(s32)(s16)mem_read16(cpu, ea);
    *pc = cia + 4u;
    return 1;
  }
  if (op == 10u) {
    u32 b = ra ? cpu->gpr[ra] : 0u;
    u32 crfd = rd >> 2;
    u32 c = 0;
    if (b < simm)
      c = 8;
    else if (b > simm)
      c = 4;
    else
      c = 2;
    cpu->cr = (cpu->cr & ~(0xFu << (28 - crfd * 4))) | (c << (28 - crfd * 4));
    *pc = cia + 4u;
    return 1;
  }
  if (op == 11u) {
    s32 a = (s32)(ra ? cpu->gpr[ra] : 0u);
    u32 crfd = rd >> 2;
    u32 c = 0;
    if (a < simms)
      c = 8;
    else if (a > simms)
      c = 4;
    else
      c = 2;
    cpu->cr = (cpu->cr & ~(0xFu << (28 - crfd * 4))) | (c << (28 - crfd * 4));
    *pc = cia + 4u;
    return 1;
  }
  if (op == 16u) {
    u32 bd = w & 0xFFFCu;
    if (bd & 0x8000u)
      bd -= 0x10000u;
    u32 dest = ((w >> 1) & 1u) ? bd : cia + bd;
    u32 bo = rd, bi = ra;
    int ctr_ok = 1, cond_ok = 1;
    if ((bo & 4u) == 0) {
      cpu->ctr--;
      ctr_ok = ((bo & 2u) != 0) ? (cpu->ctr == 0) : (cpu->ctr != 0);
    }
    if ((bo & 16u) == 0) {
      int crbit = (cpu->cr >> (31 - bi)) & 1;
      cond_ok = ((bo & 8u) != 0) ? crbit : !crbit;
    }
    if (ctr_ok && cond_ok) {
      if (w & 1u)
        cpu->lr = cia + 4u;
      *pc = dest;
    } else
      *pc = cia + 4u;
    return 1;
  }
  return 0;
}

static u32 interp_vector(Chassis *ch, u32 vec) {
  CPUState *cpu = &ch->cpu;
  u32 pc = vec;
  for (int i = 0; i < 128; i++) {
    if (pc == 0)
      return 0;
    if (!in_vector_page(pc)) {
      if (pc >= 0x80004000u && pc < 0x81800000u)
        return pc;
      ch_logf(ch, "vector left page pc=0x%08x vec=0x%08x i=%u", pc, vec, (u32)i);
      return 0;
    }
    u32 w = mem_read32(cpu, pc);
    int rc = interp_one(ch, w, &pc);
    if (rc == 2) {
      /* The RVL stub uses rfi to enter the DOL-resident second stage. */
      if (pc >= 0x80004000u && pc < 0x81800000u)
        return pc;
      return 0;
    }
    if (rc == 0) {
      static int once;
      if (!once) {
        once = 1;
        ch_logf(ch, "vector unk ins=0x%08x pc=0x%08x vec=0x%08x", w, pc, vec);
      }
      return 0;
    }
  }
  ch_logf(ch, "vector timeout pc=0x%08x vec=0x%08x", pc, vec);
  return 0;
}

static void maybe_irq(Chassis *ch) {
  CPUState *cpu = &ch->cpu;
  if ((cpu->msr & MSR_EE) == 0)
    return;
  u32 vec = 0;
  if (ch->dec_irq)
    vec = DEC_VECTOR;
  else if (ch->flipper.pi_cause & ch->flipper.pi_mask)
    vec = PI_IRQ_VECTOR;
  if (!vec)
    return;
  u32 ins = mem_read32(cpu, vec);
  if (ins == 0x4C000064u)
    return;
  u32 gpr[32], lr, ctr, xer, cr, sprg[4], srr0, srr1, msr, saved_pc;
  memcpy(gpr, cpu->gpr, sizeof(gpr));
  lr = cpu->lr;
  ctr = cpu->ctr;
  xer = cpu->xer;
  cr = cpu->cr;
  memcpy(sprg, ch->sprg, sizeof(sprg));
  srr0 = cpu->srr0;
  srr1 = cpu->srr1;
  msr = cpu->msr;
  saved_pc = cpu->pc;
  cpu->srr0 = cpu->pc;
  cpu->srr1 = cpu->msr;
  cpu->msr &= ~MSR_EE;
  u32 dest = interp_vector(ch, vec);
  if (!dest || dest == OS_PANIC_HANDLER || dest == OS_PANIC_ENTRY) {
    static int drops;
    if (drops < 3) {
      drops++;
      ch_logf(ch, "irq drop vec=0x%08x dest=0x%08x ins=0x%08x", vec, dest, ins);
    }
    memcpy(cpu->gpr, gpr, sizeof(gpr));
    cpu->lr = lr;
    cpu->ctr = ctr;
    cpu->xer = xer;
    cpu->cr = cr;
    memcpy(ch->sprg, sprg, sizeof(sprg));
    cpu->srr0 = srr0;
    cpu->srr1 = srr1;
    cpu->msr = msr;
    cpu->pc = saved_pc;
    return;
  }
  /* The decrementer signals once, when it counts through zero; it is up to the
   * handler to reload it. Leaving the request asserted the way a level line
   * would be re-enters the handler forever and starves everything else. */
  if (vec == DEC_VECTOR) {
    ch->dec_irq = 0;
  }
  cpu->pc = dest;
  ch->irq_taken++;
  if (ch->irq_taken <= 4u || (ch->irq_taken & 0xFFu) == 0u)
    ch_logf(ch, "irq n=%u vec=0x%08x dest=0x%08x", ch->irq_taken, vec, dest);
}

/* OSFatal(fg, bg, msg) is where a failed boot-time check ends up: the SDK
 * blacks the screen and spins forever. Print the message and the caller. */
static void log_osfatal(Chassis *ch, u32 pc) {
  if (pc != OS_FATAL)
    return;
  char msg[192];
  guest_str(&ch->cpu, ch->cpu.gpr[5], msg, sizeof(msg));
  char buf[288];
  snprintf(buf, sizeof(buf),
           "OSFatal lr=0x%08x msg@0x%08x '%s' dierr=0x%08x r13=0x%08x",
           (unsigned)ch->cpu.lr, (unsigned)ch->cpu.gpr[5], msg,
           (unsigned)mem_read32(&ch->cpu, 0x806577E0u),
           (unsigned)ch->cpu.gpr[13]);
  ch_log(ch, buf);
}

/* OSPanic(file, line, fmt, ...) -- the SDK's assertion trap. It prints through
 * OSReport and then spins in PPCHalt, so catching the entry is the only way to
 * see which assertion fired. */
static void log_ospanic(Chassis *ch, u32 pc) {
  if (pc != OS_PANIC)
    return;
  char file[96], fmt[128];
  guest_str(&ch->cpu, ch->cpu.gpr[3], file, sizeof(file));
  guest_str(&ch->cpu, ch->cpu.gpr[5], fmt, sizeof(fmt));
  char buf[320];
  snprintf(buf, sizeof(buf),
           "OSPanic %s:%u '%s' r6=0x%08x r7=0x%08x lr=0x%08x", file,
           (unsigned)ch->cpu.gpr[4], fmt, (unsigned)ch->cpu.gpr[6],
           (unsigned)ch->cpu.gpr[7], (unsigned)ch->cpu.lr);
  ch_log(ch, buf);
}

/* DVDConvertPathToEntrynum(path) -- the FST lookup every DVDOpen goes
 * through. Logging the path is the only way to tell a missing file in the
 * extracted tree from a broken FST placement. */
static void log_dvd_path(Chassis *ch, u32 pc) {
  if (pc != DVD_CONVERT_PATH || !ch->dvd_log_left)
    return;
  ch->dvd_log_left--;
  char path[128];
  guest_str(&ch->cpu, ch->cpu.gpr[3], path, sizeof(path));
  u32 fst = mem_read32(&ch->cpu, 0x80000038u);
  char buf[288];
  snprintf(buf, sizeof(buf),
           "DVDConvertPathToEntrynum '%s' lr=0x%08x fst=0x%08x n=%u "
           "w0=0x%08x root0=0x%08x",
           path, (unsigned)ch->cpu.lr, (unsigned)fst,
           (unsigned)mem_read32(&ch->cpu, fst + 8u),
           (unsigned)mem_read32(&ch->cpu, fst),
           (unsigned)mem_read32(&ch->cpu, fst + 12u));
  ch_log(ch, buf);
}

static void log_bte_trace(Chassis *ch, u32 pc) {
  if (pc != BTE_TRACE_PC || !ch->bte_log_left)
    return;
  ch->bte_log_left--;
  char fmt[144];
  guest_str(&ch->cpu, ch->cpu.gpr[4], fmt, sizeof(fmt));
  char buf[256];
  snprintf(buf, sizeof(buf), "bte '%s' a=0x%x b=0x%x c=0x%x", fmt,
           (unsigned)ch->cpu.gpr[5], (unsigned)ch->cpu.gpr[6],
           (unsigned)ch->cpu.gpr[7]);
  ch_log(ch, buf);
}

static void log_osreport(Chassis *ch, u32 pc) {
  if (pc != OS_REPORT_PC)
    return;
  char fmt[96];
  guest_str(&ch->cpu, ch->cpu.gpr[3], fmt, sizeof(fmt));
  char buf[192];
  snprintf(buf, sizeof(buf), "OSReport '%s' r4=0x%x r5=0x%x", fmt,
           (unsigned)ch->cpu.gpr[4], (unsigned)ch->cpu.gpr[5]);
  ch_log(ch, buf);
}

/* The union of the SMC sites and the HLE hook entry points, as a bitmap.
 * Both were linear scans -- nineteen entries and nine -- run on every dispatch
 * for a PC that is almost never either of them, and the dispatch loop's own
 * bookkeeping is 11.4 s of the 124 s window at retraces 1100-1400. The table
 * is built from those two arrays and from nothing else, so it cannot drift
 * from what it filters, and a false positive costs exactly the scan it
 * replaced. */
enum { PC_FILTER_BITS = 13u, PC_FILTER_WORDS = (1u << PC_FILTER_BITS) / 32u };
static u32 g_pc_filter[PC_FILTER_WORDS];

static u32 pc_filter_slot(u32 pc) {
  return ((pc >> 2) * 2654435761u) >> (32u - PC_FILTER_BITS);
}

static int pc_filter_hit(u32 pc) {
  const u32 s = pc_filter_slot(pc);
  return (int)((g_pc_filter[s >> 5] >> (s & 31u)) & 1u);
}

static void pc_filter_build(void) {
  unsigned i;
  memset(g_pc_filter, 0, sizeof(g_pc_filter));
  for (i = 0; i < sizeof(k_smc_sites) / sizeof(k_smc_sites[0]); i++) {
    const u32 s = pc_filter_slot(k_smc_sites[i]);
    g_pc_filter[s >> 5] |= 1u << (s & 31u);
  }
  for (i = 0; i < sizeof(k_hle) / sizeof(k_hle[0]); i++) {
    const u32 s = pc_filter_slot(k_hle[i].pc);
    g_pc_filter[s >> 5] |= 1u << (s & 31u);
  }
}

static u32 smc_tick(Chassis *ch, u32 pc) {
  u32 n = 0;
  for (unsigned i = 0; i < sizeof(k_smc_sites) / sizeof(k_smc_sites[0]); i++) {
    if (pc == k_smc_sites[i])
      n++;
  }
  ch->smc_hits += n;
  return n;
}

static void setup_wii_globals(Chassis *ch, u32 bss_addr, u32 bss_size) {
  CPUState *cpu = &ch->cpu;
  u32 arena_lo = (bss_addr + bss_size + 0x20000u + 31u) & ~31u;
  if (arena_lo < GC_RAM_BASE || arena_lo > WII_ARENA_HI)
    arena_lo = 0x806E0000u;

  /* Disc ID at 0x80000000 (apploader / OSGetGXRMode). */
  poke32(cpu, 0x80000000u, 0x524D4745u); /* RMGE */
  poke32(cpu, 0x80000004u, 0x30310000u); /* 01 */
  poke32(cpu, 0x80000018u, 0x5D1C9EA3u); /* Wii disc magic */
  poke32(cpu, 0x80003180u, 0x524D4745u);

  poke32(cpu, LOMEM_BOOT_MAGIC, 0x0D15EA5Eu);
  poke32(cpu, LOMEM_BOOT_VERSION, 1u);
  poke32(cpu, LOMEM_PHYS_MEM, GC_MAIN_RAM_SIZE);
  poke32(cpu, LOMEM_CONSOLE_TYPE, WII_CONSOLE_TYPE);
  poke32(cpu, LOMEM_ARENA_LO, arena_lo);
  poke32(cpu, LOMEM_ARENA_HI, WII_ARENA_HI);
  poke32(cpu, LOMEM_VIDEO_MODE, 0u); /* NTSC */
  poke32(cpu, LOMEM_SIM_MEM, GC_MAIN_RAM_SIZE);
  poke32(cpu, 0x800000F4u, 0x8179B500u);
  poke32(cpu, LOMEM_BUS_CLOCK, WII_BUS_CLOCK);
  poke32(cpu, LOMEM_CPU_CLOCK, WII_CPU_CLOCK);
  poke32(cpu, 0x800030D8u, 0xFFFFFFFFu);
  poke32(cpu, 0x80003184u, 0x80000000u);
  poke16(cpu, 0x800030E6u, 0x8201u);

  /* IOS 33 lo-mem (Dolphin VersionInfo). IPC arena is in MEM2. */
  poke32(cpu, 0x80003100u, GC_MAIN_RAM_SIZE);     /* MEM1 size */
  poke32(cpu, 0x80003104u, GC_MAIN_RAM_SIZE);     /* MEM1 sim */
  poke32(cpu, 0x80003108u, 0x81800000u);          /* MEM1 end */
  poke32(cpu, 0x8000310Cu, 0);                    /* MEM1 arena begin */
  poke32(cpu, 0x80003110u, 0x81800000u);          /* MEM1 arena end */
  poke32(cpu, 0x80003114u, 0xDEADBEEFu);
  poke32(cpu, 0x80003118u, WII_MEM2_SIZE);        /* MEM2 size */
  poke32(cpu, 0x8000311Cu, WII_MEM2_SIZE);        /* MEM2 sim */
  poke32(cpu, 0x80003120u, 0x93600000u);          /* MEM2 end */
  poke32(cpu, 0x80003124u, 0x90000800u);          /* MEM2 arena begin */
  poke32(cpu, 0x80003128u, 0x935E0000u);          /* MEM2 arena end */
  poke32(cpu, 0x8000312Cu, 0xDEADBEEFu);
  poke32(cpu, 0x80003130u, 0x935E0000u);          /* IPC buffer begin */
  poke32(cpu, 0x80003134u, 0x93600000u);          /* IPC buffer end */
  poke32(cpu, 0x80003138u, 0x00000011u);          /* Hollywood revision */
  poke32(cpu, 0x8000313Cu, 0xDEADBEEFu);
  poke32(cpu, 0x80003140u, 0x00210E18u);          /* IOS 33 version */
  poke32(cpu, 0x80003144u, 0x00030110u);          /* IOS date */
  poke32(cpu, 0x80003148u, 0x93600000u);          /* IOS reserved begin */
  poke32(cpu, 0x8000314Cu, 0x93620000u);          /* IOS reserved end */
  poke32(cpu, 0x80003150u, 0xDEADBEEFu);
  poke32(cpu, 0x80003154u, 0xDEADBEEFu);
  poke32(cpu, 0x80003158u, 0x0000FF01u);          /* RAM vendor */
  cpu->ram[0x315C] = 0x80;
  cpu->ram[0x315D] = 0xAD;
  poke16(cpu, 0x8000315Eu, 0x0113u);
  poke32(cpu, 0x80003160u, 0);

  /* rfi stubs at exception vectors until OSInit installs real ones */
  for (u32 v = 0x100u; v <= 0xF00u; v += 0x100u)
    poke32(cpu, GC_RAM_BASE + v, 0x4C000064u);
}

int chassis_init(Chassis *ch, void *mem1, u32 mem1_size, void *mem2,
                 u32 mem2_size, ChassisLogFn log, void *log_user) {
  memset(ch, 0, sizeof(*ch));
  pc_filter_build();
  ch->log = log;
  ch->log_user = log_user;
  if (!mem1 || mem1_size < GC_MAIN_RAM_SIZE)
    return 0;
  ch->cpu.ram = (u8 *)mem1;
  ch->cpu.ram_size = mem1_size;
  ch->cpu.mem2 = (u8 *)mem2;
  ch->cpu.mem2_size = mem2_size;
  memset(ch->cpu.ram, 0, ch->cpu.ram_size);
  if (ch->cpu.mem2 && ch->cpu.mem2_size)
    memset(ch->cpu.mem2, 0, ch->cpu.mem2_size);

  ios_init(&ch->ios, log, log_user);
  /* Armed, but it folds nothing until a real dispatch has told it what a
   * slice of the idle wait costs. */
  ch->idle_skip = 1;
  ch->dvd_log_left = 64;
  ch->bte_log_left = 4096;
  flipper_init(&ch->flipper);
  ch->flipper.ios = &ch->ios;
  /* The command processor resolves display lists and indexed attributes
   * straight out of guest RAM, so it adopts the same MEM1/MEM2 the CPU sees
   * rather than keeping a copy. Both stay owned by the caller. */
  ch->gx = gx_cp_create(mem1, mem1_size, mem2, mem2_size);
  gx_cp_set_phase_slot(&ch->phase, CHASSIS_PH_GX, CHASSIS_PH_GXVTX,
                       CHASSIS_PH_GXSUB, CHASSIS_PH_GXCOPY, CHASSIS_PH_GXDL);
  if (ch->gx == NULL)
    return 0;

  ch->cpu.external_user_data = ch;
  ch->cpu.external_read = ext_read;
  ch->cpu.external_write = ext_write;
  ch->cpu.external_read32 = ext_read32;
  ch->cpu.external_write32 = ext_write32;
  ch->cpu.instruction_fallback = fallback;
  ch->cpu.spr_read = spr_read;
  ch->cpu.spr_write = spr_write;
  ch->cpu.msr = 0x00002032u;  /* FP | ME | RI */
  ch->cpu.hid2 = 0xE0000000u; /* LSQE | PSE | WPE */
  ch->cpu.downcount = 0;
  ch->dec = 0xFFFFFFFFu;
  ch_logf(ch, "chassis MEM1=%u MEM2=%u broadway=%u", mem1_size, mem2_size,
          DOLRECOMP_CPU_BROADWAY);
  return 1;
}

int chassis_attach_media(Chassis *ch, const char *game_root,
                         const char *nand_root) {
  int ok = 1;
  if (game_root && *game_root) {
    ch->have_disc = disc_open(&ch->disc, game_root);
    disc_set_phase_slot(&ch->phase, CHASSIS_PH_DISC);
    if (!ch->have_disc) {
      ch_log(ch, "disc_open failed");
      ok = 0;
    } else {
      ch_logf(ch, "disc files=%u fst=0x%x dol=0x%x", ch->disc.count,
              ch->disc.fst_size, (u32)ch->disc.dol_offset);
    }
  }
  if (nand_root && *nand_root) {
    ch->have_nand = nand_open(&ch->nand, nand_root);
    if (!ch->have_nand) {
      ch_log(ch, "nand_open failed");
      ok = 0;
    }
  }
  ios_attach(&ch->ios, ch->have_disc ? &ch->disc : NULL,
             ch->have_nand ? &ch->nand : NULL);
  return ok;
}

/* What the real apploader leaves behind before it branches to the DOL entry:
 * the disc header in low memory, the FST parked at the top of MEM1 with
 * 0x80000038/0x3C pointing at it, the arena closed below it, the data
 * partition's type/offset at 0x3194/0x3198, and setting.txt at 0x80003800.
 * The DOL sections themselves are already mapped by chassis_load_dol. */
static void apploader_hle(Chassis *ch) {
  CPUState *cpu = &ch->cpu;

  if (ch->have_disc) {
    /* BS2 copies the first 0x20 bytes of the disc header to 0x80000000 and
     * the 4-byte game ID to 0x80003180. */
    for (u32 i = 0; i < 0x20u; i++)
      mem_write8(cpu, 0x80000000u + i, ch->disc.boot[i]);
    for (u32 i = 0; i < 4u; i++)
      mem_write8(cpu, 0x80003180u + i, ch->disc.boot[i]);

    u32 fst_size = ch->disc.fst_size;
    u32 fst_max = ch->disc.fst_max_size < fst_size ? fst_size
                                                   : ch->disc.fst_max_size;
    /* A real apploader parks the FST at the top of MEM1 and lowers arena hi.
     * RMGE01's OSInit recomputes arena hi as the end of MEM1 and ignores what
     * we leave at 0x80000034, so anything up there is inside the game's heap
     * and gets cleared the moment JKRHeap takes the arena -- which is what
     * left DVDConvertPathToEntrynum reading a zeroed FST. Arena *lo* is
     * honoured, so put the FST at the bottom of the arena and push lo past it
     * instead. */
    u32 addr = mem_read32(cpu, LOMEM_ARENA_LO);
    for (u32 i = 0; i < fst_size; i++)
      mem_write8(cpu, addr + i, ch->disc.fst[i]);
    poke32(cpu, 0x80000038u, addr);
    poke32(cpu, 0x8000003Cu, fst_max);
    poke32(cpu, LOMEM_ARENA_LO, (addr + fst_max + 31u) & ~31u);
    ch->fst_addr = addr;
    ch->fst_size = fst_size;

    /* Partition type (0 = game data) and offset >> 2, as the System Menu
     * leaves them for the running title. */
    poke32(cpu, 0x80003194u, 0u);
    poke32(cpu, 0x80003198u, (u32)(ch->disc.partition_offset >> 2));
    ch_logf(ch, "apploader fst=0x%08x size=0x%x arena_lo=0x%08x", addr,
            fst_size, mem_read32(cpu, LOMEM_ARENA_LO));
  }

  if (ch->have_nand) {
    /* setting.txt lives at 0x80003800 as a 256-byte blob. */
    s32 h = nand_file_open(&ch->nand,
                           "/title/00000001/00000002/data/setting.txt",
                           NAND_MODE_READ);
    if (h >= 0) {
      u8 buf[256];
      memset(buf, 0, sizeof(buf));
      s32 got = nand_file_read(&ch->nand, h, buf, sizeof(buf));
      nand_file_close(&ch->nand, h);
      for (u32 i = 0; i < sizeof(buf); i++)
        mem_write8(cpu, 0x80003800u + i, buf[i]);
      ch_logf(ch, "apploader setting.txt %d bytes -> 0x80003800", (u32)got, 0,
              0);
    } else {
      ch_log(ch, "apploader: no setting.txt in the NAND tree");
    }
  }
}

int chassis_load_dol(Chassis *ch, const char *path) {
  DOLFile dol;
  if (!dol_load(&dol, path)) {
    ch_log(ch, "dol_load failed");
    return 0;
  }
  ch_logf(ch, "dol entry=0x%08x bss=0x%08x size=0x%08x", dol.header.entry_point,
          dol.header.bss_address, dol.header.bss_size);

  /* DOL BSS is a bounding box. Initialized .sdata/.sbss overlays sit
   * inside it (RMGE01: /dev/di at 0x8069E1E8). Zero first, then copy
   * sections so the overlay wins. */
  if (!zero_guest(ch, dol.header.bss_address, dol.header.bss_size)) {
    dol_free(&dol);
    return 0;
  }
  for (int i = 0; i < DOL_NUM_TEXT; i++) {
    const u8 *p = dol_get_text_section(&dol, i);
    if (!p)
      continue;
    if (!map_bytes(ch, dol.header.text_addresses[i], p,
                   dol.header.text_sizes[i])) {
      dol_free(&dol);
      return 0;
    }
  }
  for (int i = 0; i < DOL_NUM_DATA; i++) {
    const u8 *p = dol_get_data_section(&dol, i);
    if (!p)
      continue;
    if (!map_bytes(ch, dol.header.data_addresses[i], p,
                   dol.header.data_sizes[i])) {
      dol_free(&dol);
      return 0;
    }
  }

  setup_wii_globals(ch, dol.header.bss_address, dol.header.bss_size);
  apploader_hle(ch);
  ch->cpu.pc = dol.header.entry_point;
  ch->cpu.gpr[1] = 0x817FF000u;
  dol_free(&dol);
  ch_logf(ch, "dol mapped pc=0x%08x sp=0x%08x", ch->cpu.pc, ch->cpu.gpr[1], 0);
  return 1;
}

/* Credit `used` emulated cycles to everything that runs off them.
 *
 * Lifted out of the dispatch loop unchanged -- the extraction was proved
 * equivalent at a fixed block count before anything else touched it -- so that
 * an idle skip could make emulated time pass without executing anything. The
 * skip itself did not survive its gate and is not here; this is what is left,
 * and it is kept because the loop reads better with the bookkeeping named.
 *
 * What the skip would have done, and why it is not in the tree, is recorded in
 * PROGRESS.md under "The idle skip and the bound-256 cliff". The short version:
 * spinning the guest's three-instruction wait loop is measurably wasteful, a
 * bounded skip of it gains 2-5%, and at a bound somewhere between 64 and 256
 * the boot stops progressing entirely for reasons nobody has explained. A gain
 * that small does not buy a cliff that sharp. */
static void advance_time(Chassis *ch, s64 used) {
  ch->phase = CHASSIS_PH_DISPATCH;
  if (used < 1)
    used = 1;
  ch->cycles += (u64)used;
  /* The timebase and the decrementer tick at a twelfth of the core clock
   * (the 243 MHz bus divided by four), not once per dispatch cycle. */
  ch->tb_frac += (u32)used;
  u32 ticks = ch->tb_frac / CPU_CYCLES_PER_TB;
  ch->tb_frac -= ticks * CPU_CYCLES_PER_TB;
  ch->cpu.timebase += ticks;
  if (ch->dec <= ticks) {
    ch->dec -= ticks;
    ch->dec_irq = 1;
    ch->dec_asserts++;
  } else
    ch->dec -= ticks;
  /* The remote reports at 200 Hz whether or not the game asks, and its
   * packets only reach the guest through a USB read it left parked, so both
   * the clock and the delivery run off emulated time. */
  ch->bt_frac += (u32)used;
  if (ch->bt_frac >= CPU_CYCLES_PER_WIIMOTE_SAMPLE) {
    ch->bt_frac -= CPU_CYCLES_PER_WIIMOTE_SAMPLE;
    if (ch->input_fn)
      ch->input_fn(&ch->ios.bt.input, ch->flipper.retrace, ch->input_user);
    bt_tick(&ch->ios.bt);
  }
  ch->phase = CHASSIS_PH_IOS;
  ios_bt_service(&ch->ios, &ch->flipper, &ch->cpu);
  flipper_advance(&ch->flipper, (u64)used);
  maybe_irq(ch);
  ch->phase = CHASSIS_PH_DISPATCH;
}

int chassis_run(Chassis *ch, u32 max_blocks) {
  u32 blocks = 0;
  u32 prev_pc = 0;
  int have_prev = 0;
  while (max_blocks == 0u || blocks < max_blocks) {
    if (ch->retrace_cap && ch->flipper.retrace >= ch->retrace_cap) {
      ch->stop_reason = CHASSIS_STOP_CAP;
      return 1;
    }
    u32 pc = ch->cpu.pc;
    if (have_prev && pc == prev_pc &&
        memcmp(ch->cpu.gpr, ch->spin_gpr, sizeof(ch->cpu.gpr)) == 0)
      ch->spin_count++;
    else
      ch->spin_count = 0;
    prev_pc = pc;
    memcpy(ch->spin_gpr, ch->cpu.gpr, sizeof(ch->cpu.gpr));
    have_prev = 1;
    ch->last_pc = pc;
    ch->last_lr = ch->cpu.lr;
    ch->last_ctr = ch->cpu.ctr;
    if (is_ppchalt(pc)) {
      char a[80], b[80];
      guest_str(&ch->cpu, ch->cpu.gpr[3], a, sizeof(a));
      guest_str(&ch->cpu, ch->cpu.gpr[4], b, sizeof(b));
      ch->stop_reason = CHASSIS_STOP_SPIN;
      ch_logf(ch, "PPCHalt pc=0x%08x lr=0x%08x hits=%u", pc, ch->cpu.lr,
              ch->dispatch_hits);
      {
        char buf[192];
        snprintf(buf, sizeof(buf),
                 "  r3='%s' r4='%s' r3=0x%x r4=0x%x r5=0x%x r6=0x%x", a, b,
                 (unsigned)ch->cpu.gpr[3], (unsigned)ch->cpu.gpr[4],
                 (unsigned)ch->cpu.gpr[5], (unsigned)ch->cpu.gpr[6]);
        ch_log(ch, buf);
      }
      ch_logf(ch, "  ios opens=%u ioctls=%u replies=%u", ch->ios.opens,
              ch->ios.ioctls, ch->ios.replies);
      return 1;
    }
    if (ch->trace_left && blocks >= ch->trace_from && pc >= ch->trace_lo &&
        pc < ch->trace_hi) {
      ch->trace_left--;
      ch_logf(ch, "trace 0x%08x lr=0x%08x blocks=%u", pc, ch->cpu.lr, blocks);
    }
    log_bte_trace(ch, pc);
    log_osreport(ch, pc);
    log_osfatal(ch, pc);
    log_ospanic(ch, pc);
    log_dvd_path(ch, pc);
    if (pc == OS_INIT_PC && ch->dispatch_hits < 8u)
      ch_log(ch, "enter OSInit");
    if (pc == OS_INIT_IPC)
      ch_logf(ch, "enter IPCCltInit hits=%u ipc_ready=%u y2=%u",
              ch->dispatch_hits, (u32)ch->flipper.ipc_ready,
              ch->flipper.ipc_ctrl);
    if (pc == MAIN_PC)
      ch_logf(ch, "enter main hits=%u fifo=%u", ch->dispatch_hits,
              (u32)ch->fifo_hit, 0);
    if (pc == OS_SLEEP_THREAD)
      ch_logf(ch, "OSSleepThread q=0x%08x lr=0x%08x hits=%u", ch->cpu.gpr[3],
              ch->cpu.lr, ch->dispatch_hits);
    if (pc == IOS_OPEN_SYNC || pc == IOS_OPEN_ASYNC)
      ios_note_open_arg(&ch->ios, &ch->cpu, ch->cpu.gpr[3]);
    if (pc == OS_IDLE_WAIT) {
      if (!ch->idle_note) {
        ch->idle_note = 1;
        ch->idle_at = ch->dispatch_hits;
        ch_logf(ch, "idle wait msr=0x%08x pi=0x%08x mask=0x%08x", ch->cpu.msr,
                ch->flipper.pi_cause, ch->flipper.pi_mask);
        ch_logf(ch, "  dec=0x%08x vec500=0x%08x r13=0x%08x", ch->dec,
                mem_read32(&ch->cpu, PI_IRQ_VECTOR), ch->cpu.gpr[13]);
        {
          u32 r13 = ch->cpu.gpr[13];
          u32 rq = mem_read32(&ch->cpu, r13 - 8048u);
          u32 idle = mem_read32(&ch->cpu, 0x800000E4u);
          u32 ctx = mem_read32(&ch->cpu, 0x800000C0u);
          u32 slr = mem_read32(&ch->cpu, ch->cpu.gpr[1] + 20u);
          ch_logf(ch, "  runq=0x%08x idle_cb=0x%08x ctx=0x%08x", rq, idle, ctx);
          ch_logf(ch, "  saved_lr=0x%08x ipc_ready=%u opens=%u", slr,
                  (u32)ch->flipper.ipc_ready, ch->ios.opens);
        }
      }
      ch->spin_count = 0;
      if (ch->dispatch_hits - ch->idle_at >= 16384u && ch->ios.opens == 0 &&
          !ch->fifo_hit) {
        ch->stop_reason = CHASSIS_STOP_CAP;
        ch_logf(ch, "idle wait, no runnable thread hits=%u fifo=%u",
                ch->dispatch_hits, (u32)ch->fifo_hit, 0);
        return 1;
      }
    }
    if (ch->spin_count >= SPIN_SAME) {
      ch->stop_reason = CHASSIS_STOP_SPIN;
      ch_logf(ch, "spin pc=0x%08x hits=%u mmio_w=%u", pc, ch->dispatch_hits,
              ch->mmio_writes);
      return 1;
    }
    /* Nothing below can fire for a PC the filter does not name, and
     * `smc_hits` only grows inside `smc_tick`, so the storm check is still
     * taken on the dispatch that would have tripped it. */
    if (pc_filter_hit(pc)) {
      smc_tick(ch, pc);
      if (ch->smc_hits > 256u) {
        ch->stop_reason = CHASSIS_STOP_SMC;
        ch_logf(ch, "SMC storm smc_hits=%u pc=0x%08x blocks=%u", ch->smc_hits,
                pc, blocks);
        return 0;
      }
      if (hle_call(ch, pc)) {
        blocks++;
        continue;
      }
    }
    {
      /* Rolling window of distinct block entries. A boot that parks in a wait
       * loop stops at one PC, which says nothing about how it got there; this
       * is the path, and it is dumped once, just before the spin detector
       * gives up. */
      static u32 trail[256];
      static u32 trail_n;
      if (!trail_n || trail[(trail_n - 1u) & 255u] != pc) {
        trail[trail_n & 255u] = pc;
        trail_n++;
      }
      if (ch->spin_count == SPIN_SAME - 1u) {
        for (u32 i = trail_n > 256u ? trail_n - 256u : 0u; i < trail_n; i++)
          ch_logf(ch, "trail %u 0x%08x", i, trail[i & 255u], 0);
      }
    }
    /* GV_CENSUS_FROM=<blocks> restarts the census once the boot is past its
     * one-off work, so the tail of a long run is not buried under it. */
    if (ch->census_from && blocks == ch->census_from) {
      memset(ch->hot, 0, sizeof(ch->hot));
      ch_logf(ch, "census restart at block %u", blocks, 0, 0);
    }
    hot_note(ch, pc);
    ch->cpu.downcount = SLICE_CYCLES;
    ch->ran_from = pc;
    ch->phase = pc == OS_IDLE_WAIT ? CHASSIS_PH_IDLE : CHASSIS_PH_GUEST;
    /* Fold the idle wait rather than run it. The run queue is re-read every
     * time, so this gives up the moment the guest has something to do. */
    int folded = 0;
    if (pc == OS_IDLE_WAIT && ch->idle_skip && ch->idle_used != 0u &&
        ((ch->idle_skips + ch->idle_checks) % IDLE_RECHECK) != 0u &&
        mem_read32(&ch->cpu,
                   ch->cpu.gpr[13] + (u32)(s32)OS_IDLE_RUNQ_OFF) == 0u)
      folded = 1;
    if (folded) {
      /* Exactly what those 3,419 iterations leave behind: r0 holding the zero
       * it kept loading, CR0 saying equal with SO copied out of XER the way
       * cmpwi does, and the pc back at the top of the loop. */
      ch->cpu.gpr[0] = 0u;
      ch->cpu.cr = (ch->cpu.cr & ~(0xFu << 28)) |
                   ((0x2u | ((ch->cpu.xer >> 31) & 1u)) << 28);
      ch->cpu.pc = OS_IDLE_WAIT;
      ch->cpu.downcount = (s64)SLICE_CYCLES - (s64)ch->idle_used;
      ch->idle_skips++;
    } else if (!dolrecomp_call(&ch->cpu, pc)) {
      ch->dispatch_misses++;
      ch->uncovered_pc = pc;
      ch->stop_reason = CHASSIS_STOP_MISS;
      ch_logf(ch, "dispatch miss pc=0x%08x hits=%u misses=%u", pc,
              ch->dispatch_hits, ch->dispatch_misses);
      ch_logf(ch, "  lr=0x%08x ctr=0x%08x r1=0x%08x", ch->cpu.lr, ch->cpu.ctr,
              ch->cpu.gpr[1]);
      ch_logf(ch, "  ran_from=0x%08x r0=0x%08x r3=0x%08x", ch->ran_from,
              ch->cpu.gpr[0], ch->cpu.gpr[3]);
      return 0;
    }
    ch->dispatch_hits++;
    blocks++;
    /* What a real slice of the idle wait costs, taken from the generated code
     * rather than assumed about it -- and re-taken periodically, so that a
     * regenerated tree cannot leave a stale number in place. */
    if (pc == OS_IDLE_WAIT && !folded && ch->idle_skip &&
        ch->cpu.pc == OS_IDLE_WAIT) {
      u32 used = (u32)((s64)SLICE_CYCLES - ch->cpu.downcount);
      u32 ea = ch->cpu.gpr[13] + (u32)(s32)OS_IDLE_RUNQ_OFF;
      if (mem_read32(&ch->cpu, ea) != 0u) {
        /* It spun the whole slice, so whatever it polls is still zero. If the
         * word at this address is not, then this is not the word it polls,
         * and folding would spin on a queue that already has work in it. */
        ch->idle_skip = 0;
        ch_logf(ch, "idle skip off: 0x%08x is not the polled word", ea, 0, 0);
      } else if (ch->idle_used == 0u) {
        ch->idle_used = used;
        ch->idle_poll_ea = ea;
        ch_logf(ch, "idle skip armed: slice=%u cycles ea=0x%08x hits=%u", used,
                ea, ch->dispatch_hits);
      } else if (used != ch->idle_used) {
        ch_logf(ch, "idle skip off: slice cost moved %u -> %u at hits=%u",
                ch->idle_used, used, ch->dispatch_hits);
        ch->idle_skip = 0;
      } else {
        ch->idle_checks++;
      }
    }
    if (ch->cpu.pc == 0u) {
      if (ch->cpu.lr >= GC_RAM_BASE &&
          ch->cpu.lr < GC_RAM_BASE + ch->cpu.ram_size) {
        if (ch->dispatch_hits < 32u || (ch->dispatch_hits & 0x3FFu) == 0u)
          ch_logf(ch, "null call from 0x%08x -> lr=0x%08x hits=%u", pc,
                  ch->cpu.lr, ch->dispatch_hits);
        ch->cpu.gpr[3] = 0;
        ch->cpu.pc = ch->cpu.lr;
      } else {
        ch_logf(ch, "pc became 0 from 0x%08x lr=0x%08x ctr=0x%08x", pc,
                ch->cpu.lr, ch->cpu.ctr);
        ch->stop_reason = CHASSIS_STOP_MISS;
        ch->uncovered_pc = 0;
        ch->dispatch_misses++;
        return 0;
      }
    }

    advance_time(ch, (s64)SLICE_CYCLES - ch->cpu.downcount);

    if (ch->cpu.exception == PPC_EXC_FP_UNAVAILABLE) {
      if (ch->dispatch_hits < 16u || (ch->dispatch_hits & 0x3FFu) == 0u)
        ch_logf(ch, "FP unavailable srr0=0x%08x msr=0x%08x hits=%u",
                ch->cpu.srr0, ch->cpu.msr, ch->dispatch_hits);
      /* Taking the exception cleared MSR[EE], IR, DR, RI... along with FP.
       * Standing in for the SDK's handler means undoing that the way its rfi
       * would: restore the saved MSR, then turn FP on. Leaving EE clear here
       * silently disables interrupts for the rest of the boot. */
      ch->cpu.msr = (ch->cpu.msr & ~MSR_RFI_MASK) |
                    (ch->cpu.srr1 & MSR_RFI_MASK) | 0x00002000u;
      ch->cpu.exception = 0;
      ch->cpu.pc = ch->cpu.srr0;
      continue;
    }
    if (ch->cpu.exception == PPC_EXC_SYSTEM_CALL) {
      if (ch->dispatch_hits < 32u || (ch->dispatch_hits & 0x3FFu) == 0u)
        ch_logf(ch, "sc srr0=0x%08x r0=0x%08x r3=0x%08x", ch->cpu.srr0,
                ch->cpu.gpr[0], ch->cpu.gpr[3]);
      ch->cpu.msr = ch->cpu.srr1 | 0x00002000u;
      ch->cpu.exception = 0;
      ch->cpu.pc = ch->cpu.srr0;
      continue;
    }
    if (ch->cpu.exception) {
      ch->exception = ch->cpu.exception;
      ch->stop_reason = CHASSIS_STOP_EXCEPTION;
      ch_logf(ch, "exception=0x%08x pc=0x%08x hits=%u", ch->cpu.exception,
              ch->cpu.pc, ch->dispatch_hits);
      return 0;
    }
    if (ch->fifo_hit && ch->stop_on_fifo) {
      ch->stop_reason = CHASSIS_STOP_FIFO;
      return 1;
    }
    if ((blocks & 0xFFFu) == 0u)
      ch_logf(ch, "dispatch progress hits=%u pc=0x%08x mmio_w=%u",
              ch->dispatch_hits, ch->cpu.pc, ch->mmio_writes);
  }
  ch->stop_reason = CHASSIS_STOP_CAP;
  ch_logf(ch, "block cap hits=%u pc=0x%08x fifo=%u", ch->dispatch_hits,
          ch->cpu.pc, (u32)ch->fifo_hit);
  return 1;
}

void chassis_shutdown(Chassis *ch) {
  if (!ch)
    return;
  if (ch->have_disc) {
    disc_close(&ch->disc);
    ch->have_disc = 0;
  }
  if (ch->have_nand) {
    nand_close(&ch->nand);
    ch->have_nand = 0;
  }
  gx_cp_destroy(ch->gx);
  ch->gx = NULL;
  ch->cpu.ram = NULL;
  ch->cpu.mem2 = NULL;
}

const char *chassis_stop_name(int reason) {
  switch (reason) {
  case CHASSIS_STOP_FIFO:
    return "fifo";
  case CHASSIS_STOP_MISS:
    return "miss";
  case CHASSIS_STOP_EXCEPTION:
    return "exception";
  case CHASSIS_STOP_CAP:
    return "cap";
  case CHASSIS_STOP_SMC:
    return "smc";
  case CHASSIS_STOP_SPIN:
    return "spin";
  default:
    return "running";
  }
}

void chassis_set_input(Chassis *ch, const BtInput *in) { ch->ios.bt.input = *in; }

void chassis_set_input_fn(Chassis *ch, ChassisInputFn fn, void *user) {
  ch->input_fn = fn;
  ch->input_user = user;
}

void chassis_set_bt_log(Chassis *ch, u32 packets) {
  ch->ios.bt_cmd_log_left = packets;
}
