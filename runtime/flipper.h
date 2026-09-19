#ifndef GALAXYVITA_FLIPPER_H
#define GALAXYVITA_FLIPPER_H

#include "common/types.h"

struct CPUState;
struct GxCp;
struct GxQueue;
struct Ios;

enum { FLIPPER_MMIO_TRACE = 48 };

typedef struct Flipper {
  u16 vi[64];
  u32 pi_cause;
  u32 pi_mask;
  /* Rising edges per interrupt source: AI, DSP, VI, PE token, PE finish, IPC.
   * A guest that is running flat out and drawing nothing is either waiting for
   * an interrupt that never comes or drowning in one that does, and the two
   * look identical from outside. Counting the edges separates them and names
   * the source. */
  u32 irq_count[6];
  /* Non-NULL once the render thread owns the command processor; the FIFO
   * writes are copied here instead of being parsed inline. */
  struct GxQueue *gx_queue;
  u64 gx_writes;
  u32 pi_fifo_base;
  u32 pi_fifo_end;
  u32 pi_fifo_wptr;
  u16 cp[0x40];
  u16 pe[8];
  /* Pixel Engine interrupt lines. Both latch until the CPU acknowledges them
   * through the PE control register, exactly as the hardware does. */
  int pe_token_irq;
  int pe_finish_irq;
  u16 mi[64];
  u16 dsp[32];
  u32 di[12];
  u32 si[16];
  u32 exi[16];
  u32 ai[4];
  /* AISCNT is not a register the CPU writes: it free-runs at the selected
   * sample rate whenever PSTAT is set. AIInit times it against the timebase to
   * work out the DSP clock, so it has to advance or the boot spins there. */
  u64 ai_acc;
  u32 ai_count;
  u32 ipc_ppcmsg;
  u32 ipc_armmsg;
  u32 ipc_ctrl;
  u32 ipc_irq_flag;
  u32 ipc_irq_mask;
  u32 gpio_out;
  u32 gpio_dir;
  u32 hw_resets;
  u32 vcount;
  u32 hcount;
  u32 retrace;
  u64 cycles;
  u32 unique_ea[FLIPPER_MMIO_TRACE];
  u32 unique_n;
  /* DSP->CPU mailbox. The DSP can have a couple of mails outstanding (the boot
   * ROM answers a single CPU mail with two), so the CPU side is a small ring
   * rather than one register. Bit 31 of a queued mail is the hardware's "mail
   * valid" flag and is added on read. */
  u32 dsp_mail[8];
  u32 dsp_mail_head;
  u32 dsp_mail_tail;
  /* Last value the CPU latched out of the mailbox. The DSP's own bit 31 is the
   * "mail valid" flag, and it is cleared here once the low half is read, so a
   * drained mailbox reads back the previous mail with the flag off. */
  /* Fractional CPU cycles not yet turned into a VI scanline. */
  u64 line_acc;
  u32 dsp_last_mail;
  int dsp_halted;
  /* IROM boot-ucode state: the 0x80F3xxxx selector whose value is expected
   * next, and the ucode it is assembling. */
  u32 dsp_boot_sel;
  u32 dsp_ucode_addr;
  u32 dsp_ucode_len;
  int dsp_ucode_loaded;
  /* Command being assembled for the running ucode: the CPU sends a word count,
   * then that many words, the first of which tags the command. */
  u32 dsp_cmd_left;
  u32 dsp_cmd_seen;
  u32 dsp_cmd_tag;
  u32 ipc_x1;
  int ipc_ready;
  u32 ipc_ack_delay;
  struct Ios *ios;
} Flipper;

int flipper_is_mmio(u32 ea);
void flipper_init(Flipper *f);
void flipper_advance(Flipper *f, u64 cycles);

/* Retire any PE events the render thread has produced since the last call.
 *
 * With the command processor on this thread an event could only ever appear
 * inside a FIFO write, so checking there was enough. With it on another one
 * the event arrives whenever the parse reaches it -- and the guest that is
 * waiting for it has, by definition, stopped writing. So this has to be
 * polled from somewhere that runs while the guest is doing nothing, which is
 * the slice boundary. Cheap and a no-op with no queue attached. */
void flipper_pe_poll(Flipper *f, struct GxCp *gx);
u64 flipper_read(Flipper *f, u32 ea, u8 size);
void flipper_write(Flipper *f, struct CPUState *cpu, struct GxCp *gx, u32 ea,
                   u64 value, u8 size);

/* Re-derive the IPC interrupt from the control bits. Exposed so the IOS side
 * can call it after posting a reply. */
void ipc_update_irq(Flipper *f);

#endif
