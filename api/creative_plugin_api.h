#pragma once
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
typedef struct CreativePluginInfo { uint32_t api_version; const char* id; const char* name; } CreativePluginInfo;
typedef int (*creative_plugin_init_fn)(void);
typedef void (*creative_plugin_shutdown_fn)(void);
#define CREATIVE_PLUGIN_API_VERSION 1u
#ifdef __cplusplus
}
#endif
