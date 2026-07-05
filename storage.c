#include "storage.h"
#include "../../log.h"
#include <stdlib.h>
#include <string.h>

#ifdef __wii__
#include <malloc.h>

struct StorageHandle {
    usbstorage_handle dev;
    uint8_t           lun;
    uint64_t          partition_offset; /* byte offset of WBFS partition on device */
};

StorageHandle *storage_open_usb(usbstorage_handle dev, uint8_t lun,
                                uint64_t partition_byte_offset)
{
    StorageHandle *h = (StorageHandle *)memalign(32, sizeof(StorageHandle));
    if (!h) return NULL;
    h->dev              = dev;
    h->lun              = lun;
    h->partition_offset = partition_byte_offset;
    return h;
}

GameshelfError storage_read(StorageHandle *h, uint64_t offset, uint32_t len, void *buf)
{
    if (!h || !buf || len == 0) return GS_ERR_IO;
    /* Translate logical offset (relative to WBFS partition) to device LBA */
    uint64_t abs_offset = h->partition_offset + offset;
    uint32_t lba        = (uint32_t)(abs_offset / 512);
    uint16_t n_sectors  = (uint16_t)(len / 512);
    if (n_sectors == 0 || (len % 512) != 0) return GS_ERR_IO;
    if (USBStorage_Read(&h->dev, h->lun, lba, n_sectors, (uint8_t *)buf) != 0)
        return GS_ERR_IO;
    return GS_OK;
}

void storage_close(StorageHandle *h)
{
    if (!h) return;
    USBStorage_Close(&h->dev);
    free(h);
}

#else /* host build stub */

struct StorageHandle {
    int _placeholder;
};

GameshelfError storage_read(StorageHandle *h, uint64_t offset, uint32_t len, void *buf)
{
    (void)h; (void)offset; (void)len; (void)buf;
    return GS_ERR_IO;
}

void storage_close(StorageHandle *h) { (void)h; }

#endif /* __wii__ */

