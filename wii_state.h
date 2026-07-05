#ifndef WII_STATE_H
#define WII_STATE_H

#ifdef __wii__

#include "storage.h"
#include "wbfs.h"

/*
 * Global Wii runtime state, defined in main.c and initialised during startup.
 * NULL if the corresponding init step failed or was not reached.
 */
extern StorageHandle *g_usb_storage;
extern WbfsContext   *g_wbfs_ctx;

#endif /* __wii__ */

#endif /* WII_STATE_H */
