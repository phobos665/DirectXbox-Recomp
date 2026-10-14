/* Xbox speakers onto the host's: see audio_speakers.h. */
#include "audio_speakers.h"

#include <string.h>

#define MINUS_3DB 0.70710678f

enum { FL, FR, FC, LFE, BL, BR, SL, SR, BC, NONE = -1 };

/* Which device channel holds each named speaker, or NONE. */
static void layout(uint32_t n, int at[9])
{
    int i;
    for (i = 0; i < 9; i++)
        at[i] = NONE;
    switch (n) {
    case 1:  break;                                   /* handled by the caller */
    case 3:  at[FL] = 0; at[FR] = 1; at[LFE] = 2; break;
    case 4:  at[FL] = 0; at[FR] = 1; at[BL] = 2; at[BR] = 3; break;
    case 5:  at[FL] = 0; at[FR] = 1; at[LFE] = 2; at[BL] = 3; at[BR] = 4; break;
    case 6:  at[FL] = 0; at[FR] = 1; at[FC] = 2; at[LFE] = 3; at[BL] = 4; at[BR] = 5; break;
    case 7:  at[FL] = 0; at[FR] = 1; at[FC] = 2; at[LFE] = 3; at[BC] = 4; at[SL] = 5; at[SR] = 6; break;
    case 8:  at[FL] = 0; at[FR] = 1; at[FC] = 2; at[LFE] = 3; at[BL] = 4; at[BR] = 5;
             at[SL] = 6; at[SR] = 7; break;
    default: at[FL] = 0; at[FR] = 1; break;           /* stereo, or unknown */
    }
}

void recomp_audio_fold_matrix(uint32_t out_channels,
                              float fold[RECOMP_SPEAKERS][RECOMP_FOLD_MAX_CHANNELS])
{
    int at[9], ch;
    memset(fold, 0, sizeof(float) * RECOMP_SPEAKERS * RECOMP_FOLD_MAX_CHANNELS);

    if (out_channels == 1u) {
        int s;
        for (s = 0; s < RECOMP_SPEAKERS; s++)
            if (s != RECOMP_SPEAKER_LFE)
                fold[s][0] = MINUS_3DB;
        return;
    }
    layout(out_channels, at);

    fold[RECOMP_SPEAKER_FRONT_LEFT][at[FL]] = 1.0f;
    fold[RECOMP_SPEAKER_FRONT_RIGHT][at[FR]] = 1.0f;

    if (at[FC] != NONE) {
        fold[RECOMP_SPEAKER_CENTER][at[FC]] = 1.0f;
    } else {
        fold[RECOMP_SPEAKER_CENTER][at[FL]] = MINUS_3DB;
        fold[RECOMP_SPEAKER_CENTER][at[FR]] = MINUS_3DB;
    }

    if (at[LFE] != NONE)
        fold[RECOMP_SPEAKER_LFE][at[LFE]] = 1.0f;     /* else dropped */

    ch = at[BL] != NONE ? at[BL] : at[SL];
    if (ch != NONE) fold[RECOMP_SPEAKER_BACK_LEFT][ch] = 1.0f;
    else            fold[RECOMP_SPEAKER_BACK_LEFT][at[FL]] = MINUS_3DB;

    ch = at[BR] != NONE ? at[BR] : at[SR];
    if (ch != NONE) fold[RECOMP_SPEAKER_BACK_RIGHT][ch] = 1.0f;
    else            fold[RECOMP_SPEAKER_BACK_RIGHT][at[FR]] = MINUS_3DB;
}
