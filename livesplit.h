#pragma once
#include <stdbool.h>
#include <stddef.h>

/* Minimal LiveSplit Server probe.

   dusklight's LiveSplit integration connects to a LiveSplit Server on
   127.0.0.1:16834 and pushes newline-terminated commands (starttimer, split,
   reset, ...). We stand up that listener ourselves and just record what comes
   in, so we can learn the protocol before wiring run start/end into the stats
   tracker. A background thread owns the socket; the GUI thread reads the log.
 */

void ls_start(void); /* begin listening on 127.0.0.1:16834 */
void ls_stop(void);

bool ls_listening(void); /* socket bound and accepting */
bool ls_connected(void); /* a client is currently connected */
long ls_total_lines(void);

/* recent control commands, oldest..newest in [0, ls_log_count()).
   The high-rate setgametime stream is parsed out, not logged here. */
int ls_log_count(void);
void ls_log_copy(int i, char *out, size_t n);

/* parsed run state, driven by the commands dusklight sends:
   start  = "starttimer"
   live   = "setgametime H:MM:SS.mmm" stream (authoritative game time)
   end    = stream goes quiet, or the next starttimer (no explicit stop cmd) */
bool ls_run_active(void);
long ls_start_seq(void); /* bumps on each run start; poll to detect a new run */
long ls_end_seq(void); /* bumps when a run is finalized; poll to detect end   */
long ls_game_ms(void); /* latest setgametime, ms (live run time)             */
long ls_final_ms(void); /* game time captured at the last finalize, ms        */
