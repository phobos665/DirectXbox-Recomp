/* xboxrecomp: stands in for xemu's apu_int.h, which pulls in QEMU's PCI,
 * migration and SDL headers. The APU state the voice processor works on is
 * apu_state.h's, which takes xemu's own vp/vp.h for the VP part. */
#pragma once
#include <inttypes.h>
#include <math.h>
#include "apu_state.h"
#include "fpconv.h"

/* xemu's apu_int.h: debug prints, off. */
#ifndef DPRINTF
#define DPRINTF(fmt, ...) do { } while (0)
#endif

#ifndef CASE_4
#define CASE_4(v, step)        \
    case (v):                  \
    case (v)+(step):           \
    case (v)+(step)*2:         \
    case (v)+(step)*3
#endif
