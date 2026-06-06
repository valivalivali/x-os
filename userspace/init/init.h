#pragma once

/* X OS Init — PID 1 entry point.
 * Called by kmain during Phase 1 transition.
 * Eventually this will be the first ring-3 process. */

void init_main(void);
