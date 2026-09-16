#ifndef GALAXYVITA_HLE_HOOKS_H
#define GALAXYVITA_HLE_HOOKS_H

/* Guest entry points the chassis answers natively instead of running the
 * recompiled PowerPC. Each entry is (guest address, C handler, SDK name).
 *
 * This list is the single source of truth for two consumers:
 *   - runtime/chassis.c builds its dispatch table from it;
 *   - scripts/patch-aot-hle.py rewrites the generated chunks so that calls to
 *     these addresses always escape to the dispatcher. DolRecomp compiles a
 *     `bl` whose target lands in the same 1024-instruction chunk as a plain
 *     `goto`, which would otherwise run straight past the hook.
 *
 * Keep the addresses RMGE01-specific; nothing here is portable to another
 * build of the game. */
#define GALAXYVITA_HLE_HOOKS(X)                                                \
  X(0x804EFB1Cu, hle_wud_is_ready, "WUDIsLinkedWBC")                           \
  X(0x803ACA54u, hle_hbm_load_module, "HomeButtonMenu::loadModule")            \
  X(0x803ACB88u, hle_hbm_void, "HBMCreateRSO")                                 \
  X(0x803ACB94u, hle_hbm_void, "HBMInitRSO")                                   \
  X(0x803ACBA0u, hle_hbm_void, "HBMCalcRSO")                                   \
  X(0x803ACBACu, hle_hbm_void, "HBMDrawRSO")                                   \
  X(0x803ACBB8u, hle_hbm_select_btn, "HBMGetSelectBtnNumRSO")                  \
  X(0x803ACBC4u, hle_hbm_void, "HBMSetAdjustFlagRSO")                          \
  X(0x803ACBD0u, hle_hbm_void, "HBMStartBlackOutRSO")

#endif
