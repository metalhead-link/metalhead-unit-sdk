/*
 * unitclap.h — between the C wrapper (unitclap.c) and the exported CLAP entry (entry.cpp).
 */
#ifndef UNITCLAP_H_
#define UNITCLAP_H_

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

bool unitclap_init(const char * pluginPath);
void unitclap_deinit(void);
const void * unitclap_get_factory(const char * factoryId);

// The AudioUnit type and subtype of plugin `index`; false to leave it out of the AUv2
bool unitclap_au_info(uint32_t index, char type[5], char subtype[5]);

#ifdef __cplusplus
}
#endif

#endif /* UNITCLAP_H_ */
