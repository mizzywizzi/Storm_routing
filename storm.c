#define SDL_MAIN_HANDLED
#include <SDL2/SDL.h>
#include <SDL2/SDL_ttf.h>
#include <SDL2/SDL_image.h>
#include <stdio.h>
#include <stdlib.h>

#define WIDTH 1000
#define HEIGHT 700

typedef struct {
    float x, y;
    int valid;
} Point;

float zoom = 1.0f;
float offsetX = 0.0f, offsetY = 0.0f;

int dragging = 0;
int lastMouseX = 0, lastMouseY = 0;

Point p1 = {0}, p2 = {0};
SDL_Texture* mapTex = NULL;
int mapWidth = 0, mapHeight = 0;

// Reference points
typedef struct { float px, py; double lat, lon; } RefPoint;
RefPoint refs[3] = {
    {1739.48f, 991.33f, 22.4, 68.96},
    {951.68f, 747.66f, 35.441618, 23.475934},
    {2610.89f, 1370.26f, 0.955198, 119.006733}
};

char infoText[256] = "";

// Render text
SDL_Texture* renderText(SDL_Renderer* r, TTF_Font* f, const char* t, SDL_Color c) {
    SDL_Surface* s = TTF_RenderText_Blended(f, t, c);
    if (!s) return NULL;
    SDL_Texture* tex = SDL_CreateTextureFromSurface(r, s);
    SDL_FreeSurface(s);
    return tex;
}

// Grid
void drawGrid(SDL_Renderer* r) {
    SDL_SetRenderDrawColor(r, 40, 40, 40, 255);
    for (int x = -2000; x <= 2000; x += 100) {
        int sx = (int)((x + offsetX) * zoom + WIDTH / 2);
        SDL_RenderDrawLine(r, sx, 0, sx, HEIGHT);
    }
    for (int y = -2000; y <= 2000; y += 100) {
        int sy = (int)((y + offsetY) * zoom + HEIGHT / 2);
        SDL_RenderDrawLine(r, 0, sy, WIDTH, sy);
    }
}

// Points
void drawPoint(SDL_Renderer* r, Point p) {
    if (!p.valid) return;
    SDL_SetRenderDrawColor(r, 255, 0, 0, 255);
    int sx = (int)((p.x + offsetX) * zoom + WIDTH / 2);
    int sy = (int)((p.y + offsetY) * zoom + HEIGHT / 2);
    SDL_Rect rect = {sx - 5, sy - 5, 10, 10};
    SDL_RenderFillRect(r, &rect);
}

// Simple interpolation
void interpolate(double* lat, double* lon, float px, float py) {
    double tX = (px - refs[1].px) / (refs[2].px - refs[1].px);
    double tY = (py - refs[1].py) / (refs[0].py - refs[1].py);
    if (tX < 0) tX = 0; if (tX > 1) tX = 1;
    if (tY < 0) tY = 0; if (tY > 1) tY = 1;
    *lon = refs[1].lon + tX * (refs[2].lon - refs[1].lon);
    *lat = refs[1].lat + tY * (refs[0].lat - refs[1].lat);
}

int main() {
    SDL_SetMainReady();
    SDL_Init(SDL_INIT_VIDEO);
    TTF_Init();
    IMG_Init(IMG_INIT_PNG);

    SDL_Window* win = SDL_CreateWindow("Map Picker",
        SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
        WIDTH, HEIGHT, SDL_WINDOW_SHOWN);

    SDL_Renderer* ren = SDL_CreateRenderer(win, -1,
        SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);

    SDL_Surface* mapSurf = IMG_Load("temp1.png");
    mapTex = SDL_CreateTextureFromSurface(ren, mapSurf);
    mapWidth = mapSurf->w;
    mapHeight = mapSurf->h;
    SDL_FreeSurface(mapSurf);

    offsetX = -mapWidth / 2.0f;
    offsetY = -mapHeight / 2.0f;

    TTF_Font* font = TTF_OpenFont("arial.ttf", 16);
    SDL_Color white = {255,255,255,255};

    int running = 1;
    while (running) {
        SDL_Event e;
        while (SDL_PollEvent(&e)) {
            if (e.type == SDL_QUIT) running = 0;

            if (e.type == SDL_MOUSEWHEEL) {
                zoom += e.wheel.y * 0.1f;
                if (zoom < 0.2f) zoom = 0.2f;
                if (zoom > 5.0f) zoom = 5.0f;
            }

            // Left click → pick
            if (e.type == SDL_MOUSEBUTTONDOWN && e.button.button == SDL_BUTTON_LEFT) {
                int mx = e.button.x;
                int my = e.button.y;

                float px = (mx - WIDTH/2)/zoom - offsetX + mapWidth/2.0f;
                float py = (my - HEIGHT/2)/zoom - offsetY + mapHeight/2.0f;

                if (!p1.valid) { p1.x = px-mapWidth/2; p1.y = py-mapHeight/2; p1.valid = 1; }
                else if (!p2.valid) { p2.x = px-mapWidth/2; p2.y = py-mapHeight/2; p2.valid = 1; }

                double lat, lon;
                interpolate(&lat, &lon, px, py);
                snprintf(infoText, sizeof(infoText),
                    "Pixel: %.2f, %.2f | lat %.6f lon %.6f",
                    px, py, lat, lon);
            }

            // Right click drag → pan
            if (e.type == SDL_MOUSEBUTTONDOWN && e.button.button == SDL_BUTTON_RIGHT) {
                dragging = 1;
                lastMouseX = e.button.x;
                lastMouseY = e.button.y;
            }

            if (e.type == SDL_MOUSEBUTTONUP && e.button.button == SDL_BUTTON_RIGHT) {
                dragging = 0;
            }

            if (e.type == SDL_MOUSEMOTION && dragging) {
                offsetX += (e.motion.x - lastMouseX) / zoom;
                offsetY += (e.motion.y - lastMouseY) / zoom;
                lastMouseX = e.motion.x;
                lastMouseY = e.motion.y;
            }

            if (e.type == SDL_KEYDOWN && e.key.keysym.sym == SDLK_c) {
                p1.valid = p2.valid = 0;
                infoText[0] = 0;
            }
        }

        SDL_SetRenderDrawColor(ren, 255,255,255,255);
        SDL_RenderClear(ren);

        SDL_Rect dst = {
            (int)((offsetX + mapWidth/2)*zoom + WIDTH/2 - mapWidth*zoom/2),
            (int)((offsetY + mapHeight/2)*zoom + HEIGHT/2 - mapHeight*zoom/2),
            (int)(mapWidth*zoom),
            (int)(mapHeight*zoom)
        };
        SDL_RenderCopy(ren, mapTex, NULL, &dst);

        drawGrid(ren);
        drawPoint(ren, p1);
        drawPoint(ren, p2);

        if (infoText[0]) {
            SDL_Texture* t = renderText(ren, font, infoText, white);
            SDL_Rect r = {10,10,0,0};
            SDL_QueryTexture(t,NULL,NULL,&r.w,&r.h);
            SDL_RenderCopy(ren,t,NULL,&r);
            SDL_DestroyTexture(t);
        }

        SDL_RenderPresent(ren);
        SDL_Delay(16);
    }

    SDL_DestroyTexture(mapTex);
    TTF_CloseFont(font);
    SDL_DestroyRenderer(ren);
    SDL_DestroyWindow(win);
    IMG_Quit(); TTF_Quit(); SDL_Quit();
    return 0;
}

