/********************
* Layer: APP
* File:  app_logger_registry.h
* Desc:  Known device tracking (address to serial mapping)
*/
#ifndef APP_LOGGER_REGISTRY_H
#define APP_LOGGER_REGISTRY_H

#include <stdint.h>
#include <stdbool.h>
#include "sl_bt_api.h"

#define MAX_KNOWN_DEVICES 32
#define SERIAL_MAX        20

// Look up serial by BLE address. Returns true if found, copies serial to out_serial.
bool registry_lookup(const bd_addr *address, char *out_serial);

// Store/update a BLE address to serial mapping.
void registry_store(const bd_addr *address, const char *serial);

#endif // LOGGER_REGISTRY_H
