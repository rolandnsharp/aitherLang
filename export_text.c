/* The runtime an exported patch carries, as text in the binary: the
 * engine's own sources, written out by `aither export` (export.c). */

const char export_rt_h[] = {
#embed "export/aither_rt.h"
  , 0
};
const char export_dsp_h[] = {
#embed "dsp.h"
  , 0
};
const char export_midi_h[] = {
#embed "midi.h"
  , 0
};
const char export_dsp_c[] = {
#embed "dsp.c"
  , 0
};
const char export_midi_state_c[] = {
#embed "midi_state.c"
  , 0
};
const char export_glue_c[] = {
#embed "export/glue.c"
  , 0
};
