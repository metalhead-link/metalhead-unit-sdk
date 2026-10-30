/*
 * The exported CLAP entry of a unit file's plugins (see unitclap.c), and clap-wrapper's AUv2
 * factory, which names each plugin as an AudioUnit. C++ because clap-wrapper's extension
 * headers are.
 */
#include <clap/clap.h>
#include <clapwrapper/auv2.h>
#include <cstring>

#include "unitclap.h"

#define AU_MANUFACTURER_CODE    "Mtlh"
#define AU_MANUFACTURER_NAME    "metalhead"

static bool getAuv2Info(const clap_plugin_factory_as_auv2 * factory, uint32_t index, clap_plugin_info_as_auv2_t * info) {
    return unitclap_au_info(index, info->au_type, info->au_subt);
}

static const clap_plugin_factory_as_auv2 auv2Factory = {
    AU_MANUFACTURER_CODE,
    AU_MANUFACTURER_NAME,
    getAuv2Info,
};

static const void * getFactory(const char * factoryId) {
    if (!strcmp(factoryId, CLAP_PLUGIN_FACTORY_INFO_AUV2)) {
        return &auv2Factory;
    }
    return unitclap_get_factory(factoryId);
}

extern "C" {
CLAP_EXPORT const clap_plugin_entry_t clap_entry = {
    CLAP_VERSION_INIT,
    unitclap_init,
    unitclap_deinit,
    getFactory,
};
}
