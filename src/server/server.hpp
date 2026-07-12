#pragma once

// Runs the HTTP playback test server.
//
// Command line:
//   webvideoplayback_test_server.exe --config <file>
//   webvideoplayback_test_server.exe --root <dir> --port <port>
int run_server(int argc, char** argv);
