#pragma once

// Runs the SDL/FFmpeg playback application.
//
// Command line:
//   webvideoplayback.exe [--performance-report] [media-path-or-url]
int run_player(int argc, char** argv);
