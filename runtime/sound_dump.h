/* Audio dump (sound_dump.c): the game's sound effects and the sound archive's waves as WAV files in mods/sounds/dump,
 * written by a thread of its own while the game runs. */
#pragma once

void sounddump_start(void); /* does nothing while a dump runs */
int sounddump_running(void);
void sounddump_progress(int *done, int *total, float *share); /* effects and wave archives done, of all; any thread */
/* After a dump: the files written (and 1), once; else 0. Host thread. */
int sounddump_finished(int *files);
