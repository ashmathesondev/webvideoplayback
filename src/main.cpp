#include <SDL3/SDL.h>

#include "player/player.hpp"

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <cstdlib>
#endif

// SDL expects the executable entrypoint translation unit to include SDL.h.
// The rest of the player is delegated to the player module.
#ifdef _WIN32
int WINAPI WinMain(HINSTANCE, HINSTANCE, LPSTR, int)
{
    return run_player(__argc, __argv);
}
#else
int main(int argc, char** argv)
{
    return run_player(argc, argv);
}
#endif
