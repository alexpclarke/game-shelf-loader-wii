#include "wii_loader.h"
#include "wii_state.h"
#include "wbfs.h"
#include "ios.h"
#include "../../log.h"
#include "../../model/game.h"
#include <string.h>

#ifdef __wii__

#include <gccore.h>
#include <stdio.h>
#include <stdint.h>

static GameshelfError wii_launch(const struct Game *game) {
    LOG_INFO("wii_loader: launching %s (%s)", game->name, game->id);

    printf("\x1b[2J\x1b[H");
    printf("%s\n%.6s\n", game->name, game->id);
    printf("------------------------\n");
    fflush(stdout);

    /* Lazy USB/WBFS init — deferred from startup so the menu appears immediately */
    if (!g_wbfs_ctx) {
        printf("cIOS: checking...\n");
        fflush(stdout);
        IosStatus ios_st = ios_preflight();
        if (ios_st != IOS_OK)
        {
            LOG_WARN("wii_loader: ios_preflight failed: %s", ios_status_message(ios_st));
            printf("cIOS: FAIL - %s\n", ios_status_message(ios_st));
            return GS_ERR_IOS;
        }
        LOG_INFO("wii_loader: ios_preflight OK");

        printf("USB: mounting...\n");
        fflush(stdout);
        g_usb_storage = ios_usb_mount();
        if (!g_usb_storage)
        {
            LOG_WARN("wii_loader: ios_usb_mount failed");
            printf("USB: FAIL\n");
            return GS_ERR_USB;
        }
        LOG_INFO("wii_loader: usb mount OK");

        printf("WBFS: reading...\n");
        fflush(stdout);
        GameshelfError werr = wbfs_open(g_usb_storage, &g_wbfs_ctx);
        if (werr != GS_OK)
        {
            LOG_WARN("wii_loader: wbfs_open failed: %s", gs_error_str(werr));
            printf("WBFS: FAIL (%s)\n", gs_error_str(werr));
            return werr;
        }
        LOG_INFO("wii_loader: wbfs open OK");
    }

    int slot = -1;
    GameshelfError err = wbfs_find_game(g_wbfs_ctx, game->id, &slot);
    if (err != GS_OK) {
        LOG_WARN("wii_loader: wbfs_find_game(%s) = %d", game->id, (int)err);
        printf("WBFS find: FAIL (err %d)\n", (int)err);
        return err;
    }
    LOG_INFO("wii_loader: found in WBFS slot %d", slot);
    printf("WBFS slot: %d\n", slot);

    /* Read disc sector 0 and verify Wii disc magic at offset 0x18. */
    static uint8_t sector0[2048];
    err = wbfs_read_sector(g_wbfs_ctx, slot, 0, sector0);
    if (err != GS_OK) {
        LOG_WARN("wii_loader: wbfs_read_sector(0) = %d", (int)err);
        printf("Sector 0: FAIL (err %d)\n", (int)err);
        return err;
    }

    uint32_t magic =
        ((uint32_t)sector0[0x18] << 24) | ((uint32_t)sector0[0x19] << 16) |
        ((uint32_t)sector0[0x1A] <<  8) |  (uint32_t)sector0[0x1B];
    int magic_ok = (magic == 0x5D1C9EA3u);
    LOG_INFO("wii_loader: disc magic %08lx (%s)",
             (unsigned long)magic, magic_ok ? "OK" : "WRONG");
    printf("Disc magic: %08lx %s\n", (unsigned long)magic, magic_ok ? "OK" : "WRONG");
    printf("Game ID in header: %.6s\n", (char *)sector0);

    if (!magic_ok) return GS_ERR_BAD_FORMAT;

    /* TODO Phase 4: disc_launch(g_wbfs_ctx, game->id) */
    printf("\n[Phase 4: disc launch coming]\n");
    fflush(stdout);
    return GS_OK;
}

#else

static GameshelfError wii_launch(const struct Game *game) {
    (void)game;
    return GS_ERR_IO;
}

#endif /* __wii__ */

static int wii_is_active(void) { return 1; }

static const char *wii_rom_suffixes[] = { ".wbfs", NULL };

const Loader wii_loader = {
    .rom_suffixes = wii_rom_suffixes,
    .scan_subdir = "wbfs",
    .is_active   = wii_is_active,
    .launch      = wii_launch,
};
