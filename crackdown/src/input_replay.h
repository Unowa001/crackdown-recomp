// crackdown - controller record/replay (hooks_input_replay.cpp)

#pragma once

#include <cstdint>
#include <functional>

namespace crackdown {

// Called on every character proxy integration (sub_822BF268): the first one
// starts the gameplay segment of a recording.
void NoteCharacterUpdate();

// True once character integrations have run steadily for 1 s (gameplay).
bool GameplayStarted();

// Total character proxy integrations so far. Unlike GameplayStarted() this
// keeps moving only while gameplay runs (it stalls in menus/loading/pause), so
// the AP bridge uses it as a live readiness signal.
uint64_t CharacterUpdateCount();

// How --input_replay closes the game when the recording ends.
void SetInputReplayQuit(std::function<void()> quit);

}  // namespace crackdown
