#define SDL_MAIN_HANDLED
#include <SDL2/SDL.h>
#include <SDL2/SDL_ttf.h>
#include <SDL2/SDL_image.h>
#include <stdio.h>
#include <stdlib.h>

#define WIDTH 1000
#define HEIGHT 700

typedef struct {
    float x, y;   // map-relative pixels
    int valid;
} Point;

float zoom = 1.0f;
float offsetX = 0.0f, offsetY = 0.0f;

Point p1 = {0}, p2 = {0};
SDL_Texture* mapTex = NULL;
int mapWidth = 0, mapHeight = 0;

// Reference points: {pixelX, pixelY} => {lat, lon}
typedef struct { float px, py; double lat, lon; } RefPoint;
RefPoint refs[3] = {
    {1739.48f, 991.33f, 22.4, 68.96},
    {951.68f, 747.66f, 35.441618, 23.475934},
    {2610.89f, 1370.26f, 0.955198, 119.006733}
};

// Text display buffer
char infoText[256] = "";

// Render text
SDL_Texture* renderText(SDL_Renderer* renderer, TTF_Font* font, const char* text, SDL_Color color) {
    SDL_Surface* surface = TTF_RenderText_Blended(font, text, color);
    if (!surface) return NULL;
    SDL_Texture* texture = SDL_CreateTextureFromSurface(renderer, surface);
    SDL_FreeSurface(surface);
    return texture;
}
// Draw grid over map
void drawGrid(SDL_Renderer* r) {
    SDL_SetRenderDrawColor(r,40, 40, 40, 255); // dark gray
    for (int x = -2000; x <= 2000; x += 100) {
        int sx = (int)((x + offsetX) * zoom + WIDTH / 2);
        SDL_RenderDrawLine(r, sx, 0, sx, HEIGHT);
    }
    for (int y = -2000; y <= 2000; y += 100) {
        int sy = (int)((y + offsetY) * zoom + HEIGHT / 2);
        SDL_RenderDrawLine(r, 0, sy, WIDTH, sy);
    }
}

// Draw points
void drawPoint(SDL_Renderer* r, Point p) {
    if (!p.valid) return;
    SDL_SetRenderDrawColor(r, 255, 0, 0, 255);
    int sx = (int)((p.x + offsetX) * zoom + WIDTH / 2);
    int sy = (int)((p.y + offsetY) * zoom + HEIGHT / 2);
    SDL_Rect rect = {sx - 5, sy - 5, 10, 10};
    SDL_RenderFillRect(r, &rect);
}

// Simple linear interpolation
void interpolate(double* lat, double* lon, float px, float py) {
    double tX = (px - refs[1].px) / (refs[2].px - refs[1].px);
    double tY = (py - refs[1].py) / (refs[0].py - refs[1].py);
    if (tX < 0) tX = 0; if (tX > 1) tX = 1;
    if (tY < 0) tY = 0; if (tY > 1) tY = 1;
    *lon = refs[1].lon + tX * (refs[2].lon - refs[1].lon);
    *lat = refs[1].lat + tY * (refs[0].lat - refs[1].lat);
}

int main(int argc, char* argv[]) {
    SDL_SetMainReady();
    SDL_Init(SDL_INIT_VIDEO);
    TTF_Init();
    IMG_Init(IMG_INIT_PNG);

    SDL_Window* win = SDL_CreateWindow("Map Pixel Picker", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, WIDTH, HEIGHT, SDL_WINDOW_SHOWN);
    SDL_Renderer* ren = SDL_CreateRenderer(win, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);

    // Load map
    SDL_Surface* mapSurf = IMG_Load("temp1.png");
    if (!mapSurf) { printf("IMG_Load failed: %s\n", IMG_GetError()); }
    else {
        mapTex = SDL_CreateTextureFromSurface(ren, mapSurf);
        mapWidth = mapSurf->w; mapHeight = mapSurf->h;
        SDL_FreeSurface(mapSurf);
        offsetX = -mapWidth / 2.0f; offsetY = -mapHeight / 2.0f;
    }

    TTF_Font* font = TTF_OpenFont("arial.ttf", 16);
    SDL_Color white = {255,255,255,255};
    int running = 1;
    const Uint8* keyState;

    while (running) {
        SDL_Event e;
        while (SDL_PollEvent(&e)) {
            if (e.type == SDL_QUIT) running = 0;

            // Zoom
            if (e.type == SDL_MOUSEWHEEL) {
                zoom += e.wheel.y * 0.1f;
                if (zoom < 0.2f) zoom = 0.2f;
                if (zoom > 5.0f) zoom = 5.0f;
            }

            // Left click points
            if (e.type == SDL_MOUSEBUTTONDOWN && e.button.button == SDL_BUTTON_LEFT) {
                int mx = e.button.x;
                int my = e.button.y;
                float px = (mx - WIDTH / 2) / zoom - offsetX + mapWidth / 2.0f;
                float py = (my - HEIGHT / 2) / zoom - offsetY + mapHeight / 2.0f;

                if (!p1.valid) { p1.x = px - mapWidth/2.0f; p1.y = py - mapHeight/2.0f; p1.valid = 1; }
                else if (!p2.valid) { p2.x = px - mapWidth/2.0f; p2.y = py - mapHeight/2.0f; p2.valid = 1; }

                double lat, lon;
                interpolate(&lat, &lon, px, py);
                snprintf(infoText, sizeof(infoText), "Clicked pixel: %.2f, %.2f  |  lat: %.6f, lon: %.6f", px, py, lat, lon);
            }

            // Reset
            if (e.type == SDL_KEYDOWN && e.key.keysym.sym == SDLK_r) {
                zoom = 1.0f; offsetX = -mapWidth/2; offsetY = -mapHeight/2;
                p1.valid = p2.valid = 0;
                infoText[0] = 0;
            }

            // Clear points
            if (e.type == SDL_KEYDOWN && e.key.keysym.sym == SDLK_c) {
                p1.valid = p2.valid = 0;
                infoText[0] = 0;
            }
        }

        // WASD panning
        keyState = SDL_GetKeyboardState(NULL);
        float panSpeed = 20.0f / zoom;
        if (keyState[SDL_SCANCODE_S]) offsetY -= panSpeed;
        if (keyState[SDL_SCANCODE_W]) offsetY += panSpeed;
        if (keyState[SDL_SCANCODE_D]) offsetX -= panSpeed;
        if (keyState[SDL_SCANCODE_A]) offsetX += panSpeed;

        SDL_SetRenderDrawColor(ren, 20,20,20,255);
        SDL_RenderClear(ren);

        // Draw map
        if (mapTex) {
            SDL_Rect dst;
            dst.w = (int)(mapWidth*zoom);
            dst.h = (int)(mapHeight*zoom);
            dst.x = (int)((offsetX + mapWidth/2)*zoom + WIDTH/2 - dst.w/2);
            dst.y = (int)((offsetY + mapHeight/2)*zoom + HEIGHT/2 - dst.h/2);
            SDL_RenderCopy(ren, mapTex, NULL, &dst);
        }

        drawPoint(ren,p1);
        drawPoint(ren,p2);
        // Draw map
if (mapTex) {
    SDL_Rect dst;
    dst.w = (int)(mapWidth*zoom);
    dst.h = (int)(mapHeight*zoom);
    dst.x = (int)((offsetX + mapWidth/2)*zoom + WIDTH/2 - dst.w/2);
    dst.y = (int)((offsetY + mapHeight/2)*zoom + HEIGHT/2 - dst.h/2);
    SDL_RenderCopy(ren, mapTex, NULL, &dst);
}

// Draw grid over map
drawGrid(ren);

// Draw points
drawPoint(ren, p1);
drawPoint(ren, p2);

        // Draw info text
        if (infoText[0] != 0) {
            SDL_Texture* t = renderText(ren, font, infoText, white);
            if (t) {
                SDL_Rect r = {10, 10, 0, 0};
                SDL_QueryTexture(t, NULL, NULL, &r.w, &r.h);
                SDL_RenderCopy(ren, t, NULL, &r);
                SDL_DestroyTexture(t);
            }
        }

        SDL_RenderPresent(ren);
        SDL_Delay(16);
    }

    if(mapTex) SDL_DestroyTexture(mapTex);
    TTF_CloseFont(font);
    SDL_DestroyRenderer(ren);
    SDL_DestroyWindow(win);
    IMG_Quit(); TTF_Quit(); SDL_Quit();
    return 0;
}
