// Menu/round-end music (originally Music.cpp): one Ogg file streamed on a
// dedicated thread so it never stutters while the game loads.
#pragma once

namespace dice::hfe::sound {

namespace music {

// Selects the track (stopping and freeing the previous one).
bool setTrack(const char* path);

// Stops, unloads the current track and selects `path` (nullptr = none).
bool playTrack(const char* path);

// Starts streaming the selected track; returns whether it is playing.
bool start();

// Stops playback. With fadeSeconds > 0 the thread fades out and exits on its
// own. Returns whether music was playing.
bool stop(float fadeSeconds);

// Pushes the current music volume to the streaming thread.
void updateVolume();

} // namespace music

} // namespace dice::hfe::sound
