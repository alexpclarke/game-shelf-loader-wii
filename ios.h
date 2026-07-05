#ifndef IOS_H
#define IOS_H

#include <stdbool.h>
#include "storage.h"

typedef enum
{
  IOS_OK = 0,
  IOS_ERR_SLOT_MISSING,
  IOS_ERR_WRONG_REVISION,
  IOS_ERR_RELOAD_FAILED,
  IOS_ERR_NOT_CIOS,
  IOS_ERR_USB_NO_DEVICE,
  IOS_ERR_USB_INIT_FAILED,
  IOS_ERR_USB_TIMEOUT,
  IOS_ERR_NO_WBFS_PARTITION,
} IosStatus;

const char *ios_status_message(IosStatus s);

/*
 * Verify cIOS slot 249 is installed and reload into it.
 * Must be called before fatInitDefault() so USB roots are available.
 * Returns IOS_OK on success; does not touch USB storage.
 */
IosStatus ios_preflight(void);

/*
 * Open USB storage and locate the WBFS partition.
 * Must be called after ios_preflight() succeeds.
 * Returns a StorageHandle on success, NULL on failure.
 * Caller owns the handle; release with storage_close().
 */
StorageHandle *ios_usb_mount(void);

#endif /* IOS_H */

