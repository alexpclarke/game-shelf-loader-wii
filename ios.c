#include "ios.h"
#include "storage.h"
#include "../../log.h"
#include <string.h>

const char *ios_status_message(IosStatus s) {
    switch (s) {
    case IOS_OK:                  return "OK";
    case IOS_ERR_SLOT_MISSING:    return "cIOS slot 249 is not installed. Run the d2x cIOS installer first.";
    case IOS_ERR_WRONG_REVISION:  return "cIOS slot 249 revision is too old. Re-run the d2x cIOS installer.";
    case IOS_ERR_RELOAD_FAILED:   return "Failed to reload into cIOS 249.";
    case IOS_ERR_NOT_CIOS:        return "IOS 249 does not appear to be a valid cIOS.";
    case IOS_ERR_USB_NO_DEVICE:   return "No USB storage device detected. Connect a USB drive and try again.";
    case IOS_ERR_USB_INIT_FAILED: return "USB storage failed to initialise.";
    case IOS_ERR_USB_TIMEOUT:     return "USB storage device timed out.";
    case IOS_ERR_NO_WBFS_PARTITION: return "No WBFS partition found on USB storage.";
    default:                      return "Unknown error.";
    }
}

#ifdef __wii__

#include <gccore.h>
#include <ogc/ios.h>
#include <ogc/es.h>
#include <ogc/usbstorage.h>
#include <stdio.h>
#include <stdlib.h>
#include <malloc.h>
#include <unistd.h>

#define CIOS_SLOT          249
#define CIOS_MIN_REVISION  65280   /* d2x v10 final */
#define USB_INIT_TIMEOUT_MS 4000
#define WBFS_MAGIC 0x57424653      /* "WBFS" */

/* ------------------------------------------------------------------ */
/* Helpers                                                              */
/* ------------------------------------------------------------------ */

/*
 * Check whether IOS slot N is installed by querying its TMD size via ES.
 * Returns the title version (revision) if found, -1 if not installed.
 */
static int get_ios_revision(u32 slot) {
    u64 title_id = (u64)0x0000000100000000ULL | slot;
    u32 tmd_size = 0;
    if (ES_GetTMDViewSize(title_id, &tmd_size) < 0 || tmd_size == 0)
        return -1;

    tmd_view *view = (tmd_view *)memalign(32, tmd_size);
    if (!view) return -1;

    int rev = -1;
    if (ES_GetTMDView(title_id, view, tmd_size) >= 0)
        rev = (int)view->title_version;

    free(view);
    return rev;
}

/*
 * Check that the currently running IOS has the USB 2.0 module present —
 * a reliable indicator that it is a cIOS rather than stock IOS.
 */
static bool has_usb2_module(void) {
    s32 fd = IOS_Open("/dev/usb/ehc", 0);
    if (fd >= 0) { IOS_Close(fd); return true; }
    return false;
}

/* ------------------------------------------------------------------ */
/* ios_preflight — verify cIOS is installed and reload into it         */
/* ------------------------------------------------------------------ */

IosStatus ios_preflight(void) {
    LOG_INFO("ios_preflight: starting");

    /* 1. Verify slot 249 is installed */
    printf("  [1/4] checking cIOS slot %d...\n", CIOS_SLOT);
    fflush(stdout);
    int rev = get_ios_revision(CIOS_SLOT);
    if (rev < 0) {
        printf("  [1/4] FAIL: not installed\n");
        fflush(stdout);
        LOG_ERROR("ios_preflight: IOS slot %d not installed", CIOS_SLOT);
        return IOS_ERR_SLOT_MISSING;
    }
    printf("  [1/4] OK (rev %d)\n", rev);
    fflush(stdout);
    LOG_INFO("ios_preflight: IOS slot %d found, revision %d", CIOS_SLOT, rev);

    /* 2. Verify revision is recent enough */
    printf("  [2/4] checking revision >= %d...\n", CIOS_MIN_REVISION);
    fflush(stdout);
    if (rev < CIOS_MIN_REVISION) {
        printf("  [2/4] FAIL: rev %d too old\n", rev);
        fflush(stdout);
        LOG_ERROR("ios_preflight: revision %d < minimum %d", rev, CIOS_MIN_REVISION);
        return IOS_ERR_WRONG_REVISION;
    }
    printf("  [2/4] OK\n");
    fflush(stdout);

    /* 3. Confirm we are running on cIOS 249.
     * We do NOT call IOS_ReloadIOS — it hangs when launched from stock IOS.
     * Instead, meta.xml declares <ios>249</ios> so HBC loads us directly
     * under cIOS 249.  If somehow we land on a different IOS, report it. */
    u32 current_ios = (u32)IOS_GetVersion();
    printf("  [3/4] running on IOS %u", current_ios);
    fflush(stdout);
    LOG_INFO("ios_preflight: running on IOS %u", current_ios);
    if (current_ios != (u32)CIOS_SLOT)
    {
        printf(" — expected %d (check meta.xml <ios> tag)\n", CIOS_SLOT);
        fflush(stdout);
        LOG_WARN("ios_preflight: not on IOS %d, USB may not work", CIOS_SLOT);
    }
    else
    {
        printf(" OK\n");
        fflush(stdout);
    }

    /* 4. Confirm it is actually a cIOS (USB 2.0 module present) */
    printf("  [4/4] checking USB 2.0 module...\n");
    fflush(stdout);
    if (!has_usb2_module()) {
        printf("  [4/4] FAIL: /dev/usb/ehc not found\n");
        fflush(stdout);
        LOG_ERROR("ios_preflight: /dev/usb/ehc not found - not a cIOS");
        return IOS_ERR_NOT_CIOS;
    }
    printf("  [4/4] OK\n");
    fflush(stdout);
    LOG_INFO("ios_preflight: cIOS confirmed (USB 2.0 module present)");

    LOG_INFO("ios_preflight: all checks passed");
    return IOS_OK;
}

/* ------------------------------------------------------------------ */
/* ios_usb_mount — open USB storage and locate the WBFS partition      */
/* ------------------------------------------------------------------ */

StorageHandle *ios_usb_mount(void) {
    LOG_INFO("ios_usb_mount: polling for USB device (timeout %d ms)", USB_INIT_TIMEOUT_MS);

    /* Poll until a USB mass storage device responds or timeout */
    usbstorage_handle dev;
    u32 elapsed = 0;
    const u32 poll_ms = 250;
    bool found = false;
    while (elapsed < USB_INIT_TIMEOUT_MS) {
        if (USBStorage_Open(&dev, 0, 0, 0) >= 0) { found = true; break; }
        usleep(poll_ms * 1000);
        elapsed += poll_ms;
        LOG_DEBUG("ios_usb_mount: waiting... %d ms elapsed", elapsed);
    }
    if (!found) {
        LOG_ERROR("ios_usb_mount: no USB device after %d ms", elapsed);
        return NULL;
    }
    LOG_INFO("ios_usb_mount: USB device found after %d ms", elapsed);

    /* Mount LUN 0 */
    u8 lun = 0;
    if (USBStorage_MountLUN(&dev, lun) < 0) {
        LOG_ERROR("ios_usb_mount: MountLUN failed");
        USBStorage_Close(&dev);
        return NULL;
    }
    LOG_INFO("ios_usb_mount: LUN %d mounted", lun);

    /* Read sector 0 to find the WBFS partition */
    u8 *buf = (u8 *)memalign(32, 512);
    if (!buf) {
        LOG_ERROR("ios_usb_mount: out of memory");
        USBStorage_Close(&dev);
        return NULL;
    }

    u64 wbfs_lba = 0;
    bool wbfs_found = false;

    if (USBStorage_Read(&dev, lun, 0, 1, buf) == 0) {
        if (buf[510] == 0x55 && buf[511] == 0xAA) {
            /* MBR present — scan the 4 primary partition entries */
            LOG_INFO("ios_usb_mount: MBR detected, scanning partition table");
            for (int i = 0; i < 4 && !wbfs_found; i++) {
                u8 *e = buf + 0x1BE + i * 16;
                /* LBA start at entry+8, little-endian u32 */
                u32 lba_start = ((u32)e[8])
                              | ((u32)e[9]  << 8)
                              | ((u32)e[10] << 16)
                              | ((u32)e[11] << 24);
                if (lba_start == 0) continue;
                LOG_DEBUG("ios_usb_mount: partition %d at LBA %u", i, lba_start);

                u8 *pbuf = (u8 *)memalign(32, 512);
                if (pbuf && USBStorage_Read(&dev, lun, lba_start, 1, pbuf) == 0) {
                    if (memcmp(pbuf, "WBFS", 4) == 0) {
                        wbfs_lba = lba_start;
                        wbfs_found = true;
                        LOG_INFO("ios_usb_mount: WBFS partition found at LBA %u", lba_start);
                    }
                }
                free(pbuf);
            }
        } else if (memcmp(buf, "WBFS", 4) == 0) {
            /* No MBR — WBFS starts at sector 0 */
            wbfs_lba = 0;
            wbfs_found = true;
            LOG_INFO("ios_usb_mount: WBFS at sector 0 (no MBR)");
        } else {
            LOG_ERROR("ios_usb_mount: sector 0 is neither MBR nor WBFS");
        }
    } else {
        LOG_ERROR("ios_usb_mount: failed to read sector 0");
    }
    free(buf);

    if (!wbfs_found) {
        LOG_ERROR("ios_usb_mount: no WBFS partition found");
        USBStorage_Close(&dev);
        return NULL;
    }

    StorageHandle *h = storage_open_usb(dev, lun, wbfs_lba * 512);
    if (!h) {
        LOG_ERROR("ios_usb_mount: storage_open_usb failed");
        USBStorage_Close(&dev);
        return NULL;
    }
    LOG_INFO("ios_usb_mount: storage handle ready, partition offset %lu bytes", (unsigned long)(wbfs_lba * 512));
    return h;
}

#else /* host build stubs */

IosStatus ios_preflight(void) { return IOS_OK; }

StorageHandle *ios_usb_mount(void) { return NULL; }

#endif /* __wii__ */
