#include "wbfs.h"
#include "../../log.h"
#include <stdlib.h>
#include <string.h>

/* -------------------------------------------------------------------------
 * Constants
 * ---------------------------------------------------------------------- */

#define WBFS_MAGIC           0x57424653UL   /* "WBFS" */
#define WII_DISC_SIZE        4699979776ULL  /* bytes; single-layer Wii disc */

/* -------------------------------------------------------------------------
 * Internal helpers
 * ---------------------------------------------------------------------- */

static uint16_t rd_be16(const uint8_t *p) {
    return (uint16_t)((uint16_t)p[0] << 8 | p[1]);
}

static uint32_t rd_be32(const uint8_t *p) {
    return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 |
           (uint32_t)p[2] << 8  | (uint32_t)p[3];
}

/* -------------------------------------------------------------------------
 * Context
 * ---------------------------------------------------------------------- */

struct WbfsContext {
    StorageHandle *storage;

    uint8_t  hd_sec_sz_s;          /* log2 of HD sector size (always 9 → 512) */
    uint8_t  wbfs_sec_sz_s;        /* log2 of WBFS block size (typically 23 → 8 MB) */
    uint32_t hd_sec_size;          /* 1 << hd_sec_sz_s  */
    uint32_t wbfs_sec_size;        /* 1 << wbfs_sec_sz_s */

    uint32_t n_wbfs_sec_per_disc;  /* number of WBFS blocks that span a full disc */
    uint32_t disc_info_sz;         /* size of one disc-info record, sector-aligned */

    uint8_t  disc_table[500];      /* copied from partition header; non-zero = occupied */

    /* Loaded per-game state (set by wbfs_find_game) */
    int       loaded_slot;         /* slot index, or -1 if none */
    uint16_t *wlba_table;          /* [n_wbfs_sec_per_disc], native-endian after load */
};

/* -------------------------------------------------------------------------
 * wbfs_open
 * ---------------------------------------------------------------------- */

GameshelfError wbfs_open(StorageHandle *storage, WbfsContext **ctx_out) {
    /* The partition header fits in one HD sector (512 bytes). */
    uint8_t header[512];
    GameshelfError err = storage_read(storage, 0, sizeof(header), header);
    if (err != GS_OK) return err;

    if (rd_be32(header) != WBFS_MAGIC) {
        LOG_ERROR("wbfs_open: bad magic %08lx", (unsigned long)rd_be32(header));
        return GS_ERR_BAD_FORMAT;
    }

    uint8_t hd_sec_sz_s   = header[8];
    uint8_t wbfs_sec_sz_s = header[9];

    if (hd_sec_sz_s < 9 || hd_sec_sz_s > 12 ||
        wbfs_sec_sz_s < hd_sec_sz_s || wbfs_sec_sz_s > 31) {
        LOG_ERROR("wbfs_open: bad sector sizes hd=%u wbfs=%u",
                  hd_sec_sz_s, wbfs_sec_sz_s);
        return GS_ERR_BAD_FORMAT;
    }

    WbfsContext *ctx = malloc(sizeof(WbfsContext));
    if (!ctx) return GS_ERR_NO_MEMORY;

    ctx->storage       = storage;
    ctx->hd_sec_sz_s   = hd_sec_sz_s;
    ctx->wbfs_sec_sz_s = wbfs_sec_sz_s;
    ctx->hd_sec_size   = 1u << hd_sec_sz_s;
    ctx->wbfs_sec_size = 1u << wbfs_sec_sz_s;

    ctx->n_wbfs_sec_per_disc =
        (uint32_t)((WII_DISC_SIZE + ctx->wbfs_sec_size - 1) / ctx->wbfs_sec_size);

    /* disc-info record = 0x100-byte disc-header copy + u16 wlba table,
     * rounded up to the next HD sector boundary. */
    uint32_t info_raw    = 0x100u + ctx->n_wbfs_sec_per_disc * 2u;
    uint32_t align_mask  = ctx->hd_sec_size - 1u;
    ctx->disc_info_sz    = (info_raw + align_mask) & ~align_mask;

    memcpy(ctx->disc_table, header + 12, 500);

    ctx->loaded_slot = -1;
    ctx->wlba_table  = NULL;

    LOG_INFO("wbfs_open: hd_sec=%u wbfs_sec=%u disc_info_sz=%u n_per_disc=%u",
             ctx->hd_sec_size, ctx->wbfs_sec_size,
             ctx->disc_info_sz, ctx->n_wbfs_sec_per_disc);

    *ctx_out = ctx;
    return GS_OK;
}

/* -------------------------------------------------------------------------
 * wbfs_find_game
 * ---------------------------------------------------------------------- */

GameshelfError wbfs_find_game(WbfsContext *ctx, const char *id, int *slot_out) {
    uint8_t *disc_info = malloc(ctx->disc_info_sz);
    if (!disc_info) return GS_ERR_NO_MEMORY;

    GameshelfError result = GS_ERR_NOT_FOUND;

    for (int s = 0; s < 500; s++) {
        if (!ctx->disc_table[s]) continue;

        /* disc-info records begin after the 1-sector partition header. */
        uint64_t offset =
            (uint64_t)ctx->hd_sec_size + (uint64_t)s * ctx->disc_info_sz;

        if (storage_read(ctx->storage, offset, ctx->disc_info_sz, disc_info)
            != GS_OK) {
            continue;
        }

        /* Game ID is the first 6 bytes of the embedded disc header. */
        LOG_DEBUG("wbfs_find_game: slot %d has '%.6s'", s, (char *)disc_info);
        if (memcmp(disc_info, id, 6) != 0) continue;

        /* Load and convert the wlba table to native endian. */
        free(ctx->wlba_table);
        ctx->wlba_table = malloc(ctx->n_wbfs_sec_per_disc * sizeof(uint16_t));
        if (!ctx->wlba_table) { result = GS_ERR_NO_MEMORY; break; }

        const uint8_t *raw = disc_info + 0x100;
        for (uint32_t i = 0; i < ctx->n_wbfs_sec_per_disc; i++) {
            ctx->wlba_table[i] = rd_be16(raw + i * 2);
        }

        ctx->loaded_slot = s;
        *slot_out        = s;
        result           = GS_OK;

        LOG_INFO("wbfs_find_game: '%.6s' at slot %d", id, s);
        break;
    }

    free(disc_info);
    return result;
}

/* -------------------------------------------------------------------------
 * wbfs_read_sector
 * ---------------------------------------------------------------------- */

GameshelfError wbfs_read_sector(WbfsContext *ctx, int slot,
                                uint32_t lba, void *buf) {
    if (ctx->loaded_slot != slot || !ctx->wlba_table) {
        return GS_ERR_NOT_FOUND;
    }

    /* Convert 2048-byte disc LBA to HD-sector units, then derive block index.
     *   hd_lba = lba * (2048 / hd_sec_size) = lba << (11 - hd_sec_sz_s)
     *   block_idx = hd_lba >> (wbfs_sec_sz_s - hd_sec_sz_s)
     *             = lba >> (wbfs_sec_sz_s - 11)
     */
    uint32_t shift        = ctx->wbfs_sec_sz_s - ctx->hd_sec_sz_s;  /* e.g. 14 */
    uint32_t hd_lba       = lba << (11u - ctx->hd_sec_sz_s);
    uint32_t block_idx    = hd_lba >> shift;
    uint32_t lba_in_block = hd_lba & ((1u << shift) - 1u);

    if (block_idx >= ctx->n_wbfs_sec_per_disc) {
        /* Past the end of the mapped disc image — treat as sparse. */
        memset(buf, 0, 2048);
        return GS_OK;
    }

    uint16_t wlba = ctx->wlba_table[block_idx];
    if (wlba == 0) {
        /* Sparse block — this disc region is not stored on the device. */
        memset(buf, 0, 2048);
        return GS_OK;
    }

    uint64_t byte_offset =
        ((uint64_t)wlba << ctx->wbfs_sec_sz_s) +
        ((uint64_t)lba_in_block << ctx->hd_sec_sz_s);

    return storage_read(ctx->storage, byte_offset, 2048, buf);
}

/* -------------------------------------------------------------------------
 * wbfs_close
 * ---------------------------------------------------------------------- */

void wbfs_close(WbfsContext *ctx) {
    if (!ctx) return;
    free(ctx->wlba_table);
    free(ctx);
}
