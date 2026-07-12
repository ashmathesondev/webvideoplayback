#include <SDL.h>

#include "player/player.hpp"

// SDL2 expects the executable entrypoint translation unit to include SDL.h.
// The rest of the player is delegated to the player module.
int main(int argc, char** argv)
{
    return run_player(argc, argv);
}
