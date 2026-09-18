#ifndef GALAXYVITA_PROFILER_H
#define GALAXYVITA_PROFILER_H

#include "chassis.h"

/* The on-device sampling profiler.
 *
 * The host chassis profiles itself with SIGPROF on an ITIMER_PROF timer and
 * reads the interrupted PC out of the signal's ucontext. None of that exists
 * here: no SIGPROF, no setitimer, no /proc/self/maps. So the emulation thread
 * publishes instead of being interrupted -- it writes one byte, ch->phase, at
 * each region boundary, and a thread on another core reads that byte and
 * ch->last_pc on a timer. The emulation thread pays a store; the sampler pays
 * everything else, on a core that was idling.
 *
 * Two histograms, because they answer different questions. The phase histogram
 * says which part of the emulator owns the wall clock. The guest-PC histogram
 * says which part of the *game* does -- and it is weighted by time, unlike the
 * existing hot_pc[] census, which counts block entries. Those two disagree by
 * a factor of twenty on the SDK idle loop, and the census is the one that is
 * wrong about cost: a block that runs to the full 10,000-cycle slice counts
 * once there, the same as a block that runs five instructions.
 *
 * Off unless prof.txt exists on the card. */

/* Reads the knob and, if asked, starts the sampler thread. Safe to call with
 * ch == NULL; it simply stays off. */
void gvprof_start(const Chassis *ch, int affinity);

/* Stop sampling. The report is still readable afterwards. */
void gvprof_stop(void);

int gvprof_on(void);

/* Appends the phase breakdown to a one-line string for the watchdog tick.
 * Percentages of samples taken, not of wall clock -- they are the same thing
 * only if the sampler kept its cadence, so the line carries the sample count
 * and the elapsed time and lets the reader check. */
void gvprof_report(char *out, unsigned n);

/* Writes the guest-PC histogram, hottest first, to its own file. Called at
 * exit and on every watchdog tick, so a run that never exits cleanly -- which
 * on this console is most of them -- still leaves one behind. */
void gvprof_dump_pcs(const char *path, unsigned top);

#endif
