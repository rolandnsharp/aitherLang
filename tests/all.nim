## Every test that doesn't touch the engine's global voice table, in
## one binary: one compile instead of sixty. Each test module's
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

import additive_inharmonic, analysis, array_multiline, array_state,
       array_values, audit, backing_compiles, block_tick, check_unheard,
       choir_block_compiles,
       codegen_kick, cutoff_clamp, def_params, delay_fractional, dho,
       each_top_level,
       elif_chain, error_source_line, guide_string, hoist, imports, lambdas,
       pixel,
       linemap, live_pain_points, lp1_hertz, midi_codegen,
       midi_hotreload, midi_keyboard_compiles, midi_polyphony,
       midi_state, midi_trig_consume, migration, migration_def_identity,
       migration_skip_poisoned, nan_recovery, negative_literals,
       noise_independent, optimized_build, pair_ops, part_park,
       pertype_keying, physical_instruments, pipe_chain, playblocks,
       prev_edge, render, reset_preserves_var, rollback, runaway_guard,
       scope_keying, shadow_play_name, sigil_state, small_syntax,
       state_arrays, stereo_haas_reverb, stereo_once, tcc_dsp, tcc_hello,
       tcc_tick, transparent_helpers_polymorphic, two_entry_tables,
       wave_autohoist

echo "all ok"
