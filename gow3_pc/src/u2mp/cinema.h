// u2mp cinema (theater): records every multiplayer match into a file and plays it back through the
// game's own replay task (TaskManagerReplay), restoring what PS3 1.10 cinema.cpp did.
//
// The PS4 1.02 build kept the replay task (RePlay / Resetting / RePlaying / Leave states), the
// replay stream reader, the recording header writer (WaitReady::Exit 0x5f2830) and the cinema list
// query (cond 69, 0x657af0). It lost the file store, the recorder (PS3 0x440c38), the stream buffer
// (PS3 NetInit), the PAGE_PROGRESS logic and the cinema menu actions. cinema.cpp restores those;
// the file format and store are in cinema_store.h; findings: u2mp/docs/CINEMA.md.
//
// Nothing has to be called from the packet layer: received events are recorded by the message
// layer's receive hook (M+0x150, the no-op 0x5e9d80 gamenet.cpp installs, which this file turns
// into the recorder) and sent events by hooks on SendNetEvent 0x644ab0 / NetSession 0x66a3d0.
// Set UNCHARTED_U2MP_CINEMA=0 to turn recording off (playback stays available).
#pragma once
#include <cstdint>

namespace u2mp {

// Checks every PS4 address it touches and installs the hooks; adds the count to `installed`.
// Call once from u2mp_install, after the identity check (install.cpp), before or after
// install_frontend.
void install_cinema(int &installed);

namespace cinema {
// Records one net event (the game's NetEvent*) into the current recording, if one is running.
// The installed hooks call this; exposed for a transport that bypasses SendNetEvent.
void record_event(void *event);
// True while a match is being recorded.
bool recording();
}  // namespace cinema

}  // namespace u2mp
