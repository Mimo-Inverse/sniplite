/* The elements under the mouse while an area is chosen (DETECT_ELEMENTS). Called by the
 * capture (ov_*.S), on its thread. */
#pragma once
#include "common.h"

void snap_begin(void);    /* choosing begins, the overlay is on the screen: a session and its thread */
void snap_end(void);      /* choosing is over: the session ends, nothing waits for its thread */
void snap_update(void);   /* O.chain, O.level and O.hl for where the mouse is */
void snap_remember(void); /* the wheel chose O.hl */
