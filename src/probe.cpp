#define SDL_MAIN_HANDLED
#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>
#include <iostream>

int main() {
    SDL_SetMainReady();
    std::cout << "Available Video Drivers:" << std::endl;
    for (int i = 0; i < SDL_GetNumVideoDrivers(); i++) {
        std::cout << "  [" << i << "] " << SDL_GetVideoDriver(i) << std::endl;
    }
    return 0;
}
