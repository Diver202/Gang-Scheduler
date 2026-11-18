#pragma once
#include "cloth.h"
#include <SDL2/SDL.h>
#include <cmath>
#include <cstdio>
#include <cstdlib>

// Opens a window and draws the cloth as a wireframe. Left/right arrows rotate, Esc quits.
class Viewer {
public:
    Viewer() {
        if (SDL_Init(SDL_INIT_VIDEO) != 0) fail("SDL_Init");
        window = SDL_CreateWindow("VBD cloth", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, size, size, 0);
        if (!window) fail("SDL_CreateWindow");
        renderer = SDL_CreateRenderer(window, -1, 0);
        if (!renderer) fail("SDL_CreateRenderer");
    }

    ~Viewer() {
        SDL_DestroyRenderer(renderer);
        SDL_DestroyWindow(window);
        SDL_Quit();
    }

    // Draws one frame (about 60 per second). Returns false once the window is closed.
    bool draw(const Cloth& cloth) {
        SDL_Event e;
        while (SDL_PollEvent(&e)) {
            if (e.type == SDL_QUIT) return false;
            if (e.type == SDL_KEYDOWN && e.key.keysym.sym == SDLK_ESCAPE) return false;
        }
        const Uint8* keys = SDL_GetKeyboardState(nullptr);
        if (keys[SDL_SCANCODE_LEFT]) yaw -= 0.03;
        if (keys[SDL_SCANCODE_RIGHT]) yaw += 0.03;

        SDL_SetRenderDrawColor(renderer, 255, 255, 255, 255);
        SDL_RenderClear(renderer);
        SDL_SetRenderDrawColor(renderer, 40, 90, 200, 255);

        int n = cloth.n;
        for (int i = 0; i < n; i++) {
            for (int j = 0; j < n; j++) {
                int ax, ay;
                project(cloth.x[i * n + j], ax, ay);
                if (j + 1 < n) line(ax, ay, cloth.x[i * n + j + 1]);
                if (i + 1 < n) line(ax, ay, cloth.x[(i + 1) * n + j]);
            }
        }

        int px, py;
        project(cloth.x[0], px, py);
        SDL_SetRenderDrawColor(renderer, 220, 30, 30, 255);
        SDL_Rect pin{px - 4, py - 4, 8, 8};
        SDL_RenderFillRect(renderer, &pin);

        SDL_RenderPresent(renderer);
        SDL_Delay(16);
        return true;
    }

private:
    static constexpr int size = 800;
    static constexpr double scale = 400;
    SDL_Window* window = nullptr;
    SDL_Renderer* renderer = nullptr;
    double yaw = -M_PI / 4;

    void project(Vec3 p, int& sx, int& sy) const {
        double across = p.x * std::cos(yaw) + p.z * std::sin(yaw);
        sx = size / 2 + (int)(across * scale);
        sy = 80 - (int)(p.y * scale);
    }

    void line(int ax, int ay, Vec3 other) {
        int bx, by;
        project(other, bx, by);
        SDL_RenderDrawLine(renderer, ax, ay, bx, by);
    }

    [[noreturn]] static void fail(const char* what) {
        fprintf(stderr, "%s failed: %s\n", what, SDL_GetError());
        exit(1);
    }
};
