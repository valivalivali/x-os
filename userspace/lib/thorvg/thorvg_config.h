#ifndef THORVG_CONFIG_H
#define THORVG_CONFIG_H

/* ThorVG build configuration for x-os freestanding environment */

#define TVG_STATIC

#define THORVG_VERSION_STRING "1.0.0"

/* POSIX string functions (strcasecmp, strncasecmp) */
#include <strings.h>

/* strtok_r is available in newlib but needs __POSIX_VISIBLE */
#undef __POSIX_VISIBLE
#define __POSIX_VISIBLE 1

/* Enabled features */
#define THORVG_CPU_ENGINE_SUPPORT
#define THORVG_SVG_LOADER_SUPPORT
#define THORVG_SFNT_LOADER_SUPPORT
#define THORVG_TTF_LOADER_SUPPORT
#define THORVG_CAPI_BINDING_SUPPORT

/* Disabled features (not available in freestanding env) */
/* THORVG_FILE_IO_SUPPORT - no filesystem */
/* THORVG_THREAD_SUPPORT - no threads */
/* THORVG_LOG_ENABLED - no logging */
/* THORVG_GL_ENGINE_SUPPORT - no OpenGL */
/* THORVG_WG_ENGINE_SUPPORT - no WebGPU */
/* THORVG_PARTIAL_RENDER_SUPPORT - not needed */
/* THORVG_PNG_LOADER_SUPPORT - not needed */
/* THORVG_JPG_LOADER_SUPPORT - not needed */
/* THORVG_LOTTIE_LOADER_SUPPORT - not needed */
/* THORVG_WEBP_LOADER_SUPPORT - not needed */
/* THORVG_GIF_SAVER_SUPPORT - not needed */
/* THORVG_AVX_VECTOR_SUPPORT - disabled for compatibility */
/* THORVG_NEON_VECTOR_SUPPORT - not x86 */

#endif /* THORVG_CONFIG_H */
