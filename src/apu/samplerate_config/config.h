/* libsamplerate's config.h for the APU's build of it (third_party/libsamplerate,
 * 0.2.2): only the fastest sinc converter, which is the one xemu's voice
 * processor asks for (SRC_SINC_FASTEST). */
#pragma once
#define PACKAGE "libsamplerate"
#define VERSION "0.2.2"
#define HAVE_STDBOOL_H 1
#define HAVE_STDINT_H 1
#define HAVE_LRINT 1
#define HAVE_LRINTF 1
#define CPU_CLIPS_POSITIVE 0
#define CPU_CLIPS_NEGATIVE 0
#define ENABLE_SINC_FAST_CONVERTER 1
