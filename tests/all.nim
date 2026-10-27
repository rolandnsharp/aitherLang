## Every test that doesn't touch the engine's global voice table, in
## one binary. What the language computes is tests/lang/*.aither, run
## by `lang`. Each test module's
## top-level code runs when it is imported, in this order, and prints
## its "ok" line; the first failing doAssert stops the run with the
## test's own message.
##
##   make test                      (or: nim c -r --hints:off tests/all.nim)
##
## The four engine tests (cli_output_format, midi_resilience,
## slot_sweep, solo_mutes) poke `engine`'s slots and stay separate
## binaries so they don't leak state into each other; `make test` runs
## them after this. `temporal_audit` is a tool, not a test.

import analysis, audit, block_tick, check_unheard, dho, guide_string,
       hoist, imports, input_gate, lang, linemap, lp1_hertz,
       midi_codegen, midi_hotreload, midi_keyboard_compiles,
       midi_polyphony, midi_state, midi_trig_consume, migration,
       migration_def_identity, migration_skip_poisoned, nan_recovery,
       optimized_build, part_park, pertype_keying,
       physical_instruments, pixel, playblocks, prev_edge,
       recursion_guard, render, reset_preserves_var, resonator_hz,
       rollback, runaway_guard, scope_keying, state_arrays, tape_heads,
       transparent_helpers_polymorphic, vdp

echo "all ok"
