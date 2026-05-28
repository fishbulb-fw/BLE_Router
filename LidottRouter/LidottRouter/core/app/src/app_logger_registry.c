/********************
* Layer: APP
* File:  app_logger_registry.c
* Desc:  Known device tracking (address to serial mapping)
*/
#include "app_logger_registry.h"
#include <string.h>

typedef struct {
    bd_addr address;
    char    serial[SERIAL_MAX];
    bool    valid;
} registry_entry_t;

static registry_entry_t known_devices[MAX_KNOWN_DEVICES];

bool registry_lookup(const bd_addr *address, char *out_serial)
{
    for (int i = 0; i < MAX_KNOWN_DEVICES; i++) {
        if (known_devices[i].valid
            && memcmp(&known_devices[i].address, address, sizeof(bd_addr)) == 0) {
            strncpy(out_serial, known_devices[i].serial, SERIAL_MAX - 1);
            out_serial[SERIAL_MAX - 1] = '\0';
            return true;
        }
    }
    return false;
}

void registry_store(const bd_addr *address, const char *serial)
{
    for (int i = 0; i < MAX_KNOWN_DEVICES; i++) {
        if (known_devices[i].valid
            && memcmp(&known_devices[i].address, address, sizeof(bd_addr)) == 0) {
            strncpy(known_devices[i].serial, serial, SERIAL_MAX - 1);
            known_devices[i].serial[SERIAL_MAX - 1] = '\0';
            return;
        }
    }
    for (int i = 0; i < MAX_KNOWN_DEVICES; i++) {
        if (!known_devices[i].valid) {
            memcpy(&known_devices[i].address, address, sizeof(bd_addr));
            strncpy(known_devices[i].serial, serial, SERIAL_MAX - 1);
            known_devices[i].serial[SERIAL_MAX - 1] = '\0';
            known_devices[i].valid = true;
            return;
        }
    }
    // Full -- overwrite slot 0
    memcpy(&known_devices[0].address, address, sizeof(bd_addr));
    strncpy(known_devices[0].serial, serial, SERIAL_MAX - 1);
    known_devices[0].serial[SERIAL_MAX - 1] = '\0';
}
