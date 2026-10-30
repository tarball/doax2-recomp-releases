#pragma once

// A rollback test: a fight played as GGPO would play it with player 2's input arriving late, on one machine.
// DEBUG_ROLLBACK=<k>[,<first>[,<last>]]: in the first fight, from its fight frame `first` (default 0) to `last` (default
// its end), player 2 (game user 1; with DEBUG_ROLLBACK_REMOTE=1, player 1) is the remote player, whose input for a frame
// becomes known k frames after the frame runs; the other is the local one. A frame whose remote input isn't known yet runs
// on a prediction: the last input known, as GGPO predicts, or where DEBUG_ROLLBACK_PREDICT=<script> has a press
// (F<frame>:<buttons>, as INPUT_SCRIPT2), that press: a wrong guess of another kind (another attack where the real input
// attacks too). Each player's real input is their script's presses in the fight's frames (INPUT_SCRIPT, INPUT_SCRIPT2),
// which both pads get from the test, with packet numbers that follow the fight frame.
//
// At each frame boundary the remote input of the frame k frames back becomes known. If it isn't what that frame ran
// with, the game rolls back: it loads the savestate of that frame's boundary (netplay/savestates.h, saved at every frame
// from `first` on) and replays the k frames to the present in replay mode (netplay/replay.h), the first with the input
// now known and the others with the prediction made from it, the journal answering the library calls the abandoned
// timeline made the same, making the others and undoing the abandoned timeline's that the new one doesn't make; then the
// present frame runs as usual. So a rollback is always k frames, and comes at every frame where player 2's input changes
// (a press and its release), as with GGPO at k frames of latency. k = 0 plays the same fight without rollbacks, with the
// same pads, savestates and journal: the reference.
//
// A fight frame's checksum (netplay::FightChecksum) is final once the inputs of the frames before it are all known: each
// is written then, once, to dumps/rollback_checksums.txt (a fight frame and its checksum a line, as fight_checksums.txt),
// and with DEBUG_ROLLBACK_REFERENCE=<path> (the reference's rollback_checksums.txt or fight_checksums.txt) compared with the
// reference's: the test passes if every frame compared is the same. And at each frame boundary that isn't a replay's,
// the sounds the engine plays (the game's GameState::soundsPlaying) are written to dumps/rollback_sounds.txt, for
// comparing the audio with the reference's (tools\netplay\rollbacksounds.py): no sound doubled or left playing.
//
// The pictures: a frame that runs as the present is shown as the reference shows it unless one of the remote inputs it
// runs on that aren't known yet was predicted wrong. Those frames are shown on a wrong prediction, as GGPO shows a frame
// until its rollback, and are written to dumps/rollback_predicted.txt (a fight frame a line), so that the test's frame
// dumps (DUMP_FRAMES=F...) compare with the reference's on the others (tools\netplay\rollback.ps1 -Frames): what the
// checksum doesn't cover (cloth, hair, effects) must be the same there too. DEBUG_ROLLBACK_SAVESTATES=0 with k = 0
// runs the fight without savestates or the journal, as the game plays it without rollback's machinery: the same as the
// reference at every frame, checksums and pictures, or the machinery changes the game (the store map's misses cleared
// the YMM registers' upper halves once: runtime/store_miss.cpp).
//
// The log has each rollback's numbers every 600 fight frames and a summary at the end, the details in
// dumps/rollback_test.txt: the rollbacks, the loads' and replays' times (a replay's first frame waits for the present
// frame's vblank, as the next frame would have), the journal's calls (answered, made, abandoned and undone, and the time
// the made and answered ones took), the vblanks the frames that ran as the present took (one a frame unless rollbacks
// didn't fit in a frame's time, and a frame was shown twice), and changes to the game's memory map.
#include <cstdint>

namespace netplay::rollbacktest
{
    // Whether a rollback test is set up (DEBUG_ROLLBACK).
    bool Enabled();
    // At each frame boundary, on the main thread, after the savestates (frame.h).
    void OnFrame(uint64_t frame);
    // The input module's scripted presses: the buttons script `script` (0 player 1's, 1 player 2's, 2 the prediction's)
    // holds at fight frame `fightFrame`.
    void SetScripts(uint16_t (*buttons)(int script, uint64_t fightFrame));
    // The pad game user `user` reads this frame, if the test decides it (users 0 and 1 in the test's fight): true, with
    // the buttons and the packet number.
    bool PadFor(uint32_t user, uint16_t& buttons, uint32_t& packetNumber);
}
