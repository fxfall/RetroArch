# ROMX 0.2.0 adapter tests

`romx_frontend_test.c` is a standalone regression suite for the ROMX
frontend/session, transparent Libretro VFS proxy and mutable SAVE adapter. It
creates all ROMX fixtures with libromx's public writer API in a caller-provided
scratch directory, so it does not depend on a particular game dump or on
fixtures from another repository.

The suite covers:

- canonical and case-insensitive `.romx` recognition;
- rejection of structurally valid containers with an unknown registry ID;
- invalid Footer, invalid RIDX and structurally valid RIDX files with no
  entrypoint;
- single-file mapping/bounded reads and core-without-VFS materialization;
- multi-file CUE VFS reads in both need-fullpath modes, stat, directory
  enumeration and virtual-path preservation;
- rejection of parent traversal, namespace-root absolute paths and native
  backslashes, including stale namespaces after teardown;
- embedded cover extraction, deterministic cache publication and missing
  metadata fallback;
- flat single-file SAVE slots (including nested paths that must not be
  auto-grouped), PSP directory slots and a Citra/Azahar 3DS Title Save
  directory candidate, including slot counts, complete member lists, stable
  IDs, export paths, rescans, import, replace, delete and capacity-failure
  rollback;
- session-delta STATS arithmetic/overflow checks and POSIX symlink-safe host
  transaction cleanup (Windows reparse-point coverage remains a CI concern);
- ordinary ROM and ZIP paths remaining on the transparent upstream VFS path,
  plus a real stored-entry ZIP list/extraction round trip through
  libretro-common's existing archive parser;
- repeated mapped, materialized, VFS and mutable-adapter lifecycles, including
  a fresh non-reused namespace after reopening a container. The repeated loop
  is intended to be run under AddressSanitizer/UndefinedBehaviorSanitizer;
  LeakSanitizer is host-dependent and is unavailable in the macOS runner.

The main coverage groups are `test_standard_recognition_and_metadata`,
`test_private_format_entrypoint`, `test_invalid_inputs`,
`test_single_mapping_and_materialization`,
`test_multi_vfs`, `test_cover_and_metadata_fallback`,
`test_flat_save_slots`, `test_non_psp_nested_files_are_independent`,
`test_psp_save_slots`, `test_n3ds_title_save_slots`, `test_stats_delta_merge`,
`test_host_transaction_symlink_safety`, `test_ordinary_and_archive_regressions`
and `test_repeated_lifecycle` (the current suite reports 137 checks).

The test intentionally stops at the adapter boundary. A real emulator core is
not loaded: core-facing `retro_game_info` data, the logical path and the VFS
callbacks are exercised through the same session APIs RetroArch uses, while
core-specific execution remains a separate integration test.

Build and run manually after building libromx 0.2.0:

```sh
make -C romx/tests ROMX_PREFIX=/path/to/libromx/build-libretro
ROMX_TEST_DIR=/path/to/a/fresh/directory make -C romx/tests run
```

For lifecycle checking, add `SANITIZER=address` (or
`SANITIZER=address,undefined`) to the build command. The suite itself is not
run by RetroArch's default build.

`make check` creates and removes a temporary directory automatically. This
repository does not run that target as part of the normal RetroArch build.
