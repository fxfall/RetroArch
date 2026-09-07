/* ROMX 0.2.0 mutable persistence bridge for RetroArch.
 *
 * This is the only layer that maps ROMX SAVE/CHEAT/STATS objects onto
 * RetroArch's ordinary host paths. Menu code must not inspect save trees or
 * call libromx directly.
 */

#ifndef RETROARCH_ROMX_PERSISTENCE_H
#define RETROARCH_ROMX_PERSISTENCE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef enum romx_persistence_write_mask
{
   ROMX_PERSISTENCE_WRITE_SAVE  = (1 << 0),
   ROMX_PERSISTENCE_WRITE_CHEAT = (1 << 1),
   ROMX_PERSISTENCE_WRITE_STATS = (1 << 2),
   ROMX_PERSISTENCE_WRITE_ALL   = ROMX_PERSISTENCE_WRITE_SAVE |
                                  ROMX_PERSISTENCE_WRITE_CHEAT |
                                  ROMX_PERSISTENCE_WRITE_STATS
} romx_persistence_write_mask_t;

/* Activates one ROMX source and restores its mutable objects to the standard
 * RetroArch locations. Existing local files are never overwritten. This must
 * run after savefile_directory has been resolved and before core_load_game().
 */
bool romx_persistence_activate(const char *romx_path, uint16_t platform_id,
      char *error_message, size_t error_message_size);

/* Clears all path/slot mappings. It never writes to the ROMX container. */
void romx_persistence_deactivate(void);

/* Removes only host files that were newly restored by the current activation,
 * then clears the bridge. Used when the subsequent core load fails. */
void romx_persistence_abort_activation(void);

bool romx_persistence_is_active(void);

/* Flushes ordinary RetroArch SRAM/cheat state when required, then imports or
 * replaces the requested namespaces. Each libromx object commit is atomic.
 */
bool romx_persistence_write_back(unsigned write_mask,
      char *message, size_t message_size);

#endif
