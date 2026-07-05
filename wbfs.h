#ifndef WBFS_H
#define WBFS_H

#include <stdint.h>
#include "../../model/loader.h"
#include "storage.h"

/*
 * Phase 3 — WBFS reader
 *
 * Reads game disc sectors from a WBFS partition via a StorageHandle.
 * Sparse blocks (wlba entry == 0) are returned as zeroed memory rather
 * than causing a device read.
 *
 * Call order:
 *   wbfs_open()       — validate partition header, populate context
 *   wbfs_find_game()  — locate slot and load wlba table for a game
 *   wbfs_read_sector()— read 2048-byte disc sectors (Phase 4 uses this)
 *   wbfs_close()      — release all resources
 */

typedef struct WbfsContext WbfsContext;

/*
 * Open a WBFS partition.
 * Reads and validates the partition header; populates the disc slot table.
 * Returns GS_OK and writes *ctx_out on success.
 */
GameshelfError wbfs_open(StorageHandle *storage, WbfsContext **ctx_out);

/*
 * Scan disc slots for the game with the given 6-character ID (e.g. "RMCE01").
 * Loads the slot's wlba table into the context for use by wbfs_read_sector.
 * Returns GS_ERR_NOT_FOUND if no slot matches.
 */
GameshelfError wbfs_find_game(WbfsContext *ctx, const char *id, int *slot_out);

/*
 * Read one 2048-byte disc sector at the given LBA for a previously found slot.
 * Sparse blocks (wlba == 0) are returned as zeroed memory.
 * lba is a 2048-byte disc sector index.
 */
GameshelfError wbfs_read_sector(WbfsContext *ctx, int slot, uint32_t lba, void *buf);

void wbfs_close(WbfsContext *ctx);

#endif /* WBFS_H */
