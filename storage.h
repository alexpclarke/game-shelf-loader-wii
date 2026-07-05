#ifndef STORAGE_H
#define STORAGE_H

#include "../../model/loader.h"
#include <stdint.h>

/*
 * Opaque handle to a block storage device (USB, SD, or file-backed stub).
 * Obtain via ios_usb_mount() (Phase 2). Passed into wbfs_open() (Phase 3).
 *
 * All offsets and lengths are in bytes. Reads must be 32-byte aligned in
 * both offset and length for DMA compatibility on Wii hardware.
 */
typedef struct StorageHandle StorageHandle;

GameshelfError storage_read(StorageHandle *h, uint64_t offset, uint32_t len, void *buf);
void           storage_close(StorageHandle *h);

#ifdef __wii__
#include <ogc/usbstorage.h>
/*
 * Wrap an already-opened usbstorage_handle into a StorageHandle.
 * partition_byte_offset is the byte offset of the WBFS partition start
 * within the USB device (used to translate logical read offsets).
 * Called by ios_usb_mount() — not called directly by other code.
 */
StorageHandle *storage_open_usb(usbstorage_handle dev, uint8_t lun,
                                uint64_t partition_byte_offset);
#endif

#endif /* STORAGE_H */

