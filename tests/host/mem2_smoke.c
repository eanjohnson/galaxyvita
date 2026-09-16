/* Prove DolRecomp Broadway CPUState maps 24 MiB MEM1 + 64 MiB MEM2. */
#include "cpu/cpu.h"
#include <stdio.h>
#include <stdlib.h>

int main(void) {
  CPUState cpu;
  if (!cpu_init(&cpu)) {
    fprintf(stderr, "cpu_init failed\n");
    return 1;
  }
  if (cpu.ram_size != GC_MAIN_RAM_SIZE) {
    fprintf(stderr, "MEM1 size %u, expected %u\n", cpu.ram_size, GC_MAIN_RAM_SIZE);
    cpu_free(&cpu);
    return 1;
  }
  if (!cpu_alloc_mem2(&cpu, WII_MEM2_SIZE)) {
    fprintf(stderr, "cpu_alloc_mem2(%u) failed\n", WII_MEM2_SIZE);
    cpu_free(&cpu);
    return 1;
  }
  if (cpu.mem2_size != WII_MEM2_SIZE) {
    fprintf(stderr, "MEM2 size %u, expected %u\n", cpu.mem2_size, WII_MEM2_SIZE);
    cpu_free(&cpu);
    return 1;
  }
  printf("broadway MEM1=%u MEM2=%u DOLRECOMP_CPU_BROADWAY=%d\n",
         cpu.ram_size, cpu.mem2_size,
#ifdef DOLRECOMP_CPU_BROADWAY
         DOLRECOMP_CPU_BROADWAY
#else
         0
#endif
  );
  cpu_free(&cpu);
  return 0;
}
