# RetroArch ROMX adapter

This directory is the only frontend integration boundary for ROMX 0.2.0.
Libretro cores continue to receive ordinary `retro_game_info` and VFS calls.

`romx_frontend` owns a launch session, payload mapping, materialized entrypoint,
metadata, cover and cleanup. `romx_vfs` translates the one Libretro VFS table
to bounded RIDX entry access. `romx_save_adapter` is the only code allowed to
project mutable SAVE bundles onto RetroArch-visible logical slots.

Save projection is selected by the platform/format profile returned by
libromx's `romx_save_catalog_*` and `romx_mutable_bundle_*` APIs; the adapter
does not infer ownership by counting directories:

- PSP marker-directory saves are grouped only after libromx validates
  `PARAM.SFO`/`SAVEDATA_DIRECTORY` identity.
- 3DS title and ExtData candidates use libromx's directory-per-save profile,
  including Gateway, SaveDataFiler and Citra/Azahar path normalization.
- Single-file profiles expose one logical slot per catalog candidate. A slash
  in a path does not turn an arbitrary non-directory-profile bundle into a
  grouped save.

Adding another directory-save format belongs in libromx's public profile API;
the RetroArch adapter must consume that profile instead of adding a private
format parser.

The frontend enumerates, inspects, imports, exports, deletes and replaces slots
only through `romx_save_adapter.h`. Local exports use same-parent staging and a
rename/rollback commit. Container writes use libromx's durable mutable-object
transaction. Replacing one slot in an aggregate bundle preserves every
unselected entry. Calls on one adapter are serialized by the frontend;
successful mutations refresh its catalog, so list indices are never cached.

The adapter boundary has a standalone regression suite in `tests/`. It creates
fresh 0.2.0 containers with libromx and exercises recognition, malformed
inputs, mapped/materialized/VFS launch views, path traversal rejection, cover
cache fallback, flat and PSP mutable slots, atomic mutation rollback, ordinary
ROM/ZIP passthrough and repeated lifecycle cleanup. See
`tests/README.md` for the manual build and run commands.
