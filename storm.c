#define SDL_MAIN_HANDLED
#include <SDL2/SDL_mixer.h>
#include <SDL2/SDL.h>
#include <SDL2/SDL_ttf.h>
#include <SDL2/SDL_image.h>
#include <stdio.h>
#include <math.h>
#include <stdlib.h>

#define WIDTH 1000
#define HEIGHT 700
#define TOPBAR 40
#define GRID_SCALE 4 
#define PADDING_COST 50.0f
#define VELOCITY_SAMPLES 5 // For smooth momentum calculation

typedef struct { float x, y; int valid; float alpha; } Point;
typedef struct { int r, c; } GridPos;
typedef struct Node {
    GridPos pos;
    float g, h, f;
    struct Node* parent;
} Node;

typedef struct { Node** nodes; int size; } MinHeap;

// --- A* Heap Functions ---
void pushHeap(MinHeap* heap, Node* node) {
    int i = heap->size++;
    while (i > 0) {
        int p = (i - 1) / 2;
        if (heap->nodes[p]->f <= node->f) break;
        heap->nodes[i] = heap->nodes[p];
        i = p;
    }
    heap->nodes[i] = node;
}

Node* popHeap(MinHeap* heap) {
    if (heap->size == 0) return NULL;
    Node* res = heap->nodes[0];
    Node* last = heap->nodes[--heap->size];
    int i = 0;
    while (i * 2 + 1 < heap->size) {
        int child = i * 2 + 1;
        if (child + 1 < heap->size && heap->nodes[child + 1]->f < heap->nodes[child]->f) child++;
        if (last->f <= heap->nodes[child]->f) break;
        heap->nodes[i] = heap->nodes[child];
        i = child;
    }
    heap->nodes[i] = last;
    return res;
}

// --- Globals ---
float zoom = 1.0f, targetZoom = 1.0f;
float camX = 0, camY = 0;

// Coordinate conversion calibration (pixel to lat/lon)
// Based on reference points:
// [541, 1320] = (35.936437, -5.610504)
// Longitude increases ~0.0574° per pixel X
// Latitude decreases ~0.0531° per pixel Y
#define REF_PIXEL_X 541.0f
#define REF_PIXEL_Y 1320.0f
#define REF_LAT 35.936437f
#define REF_LON -5.610504f
#define LON_PER_PIXEL 0.057392887f
#define LAT_PER_PIXEL -0.05314954f

// Momentum Variables
float velX = 0.0f, velY = 0.0f;
float friction = 0.94f;
float frameVelX[VELOCITY_SAMPLES] = {0};
float frameVelY[VELOCITY_SAMPLES] = {0};
int velIdx = 0;

Point p1 = {0,0,0,0}, p2 = {0,0,0,0};
SDL_Texture *mapTex = NULL, *startTex = NULL, *endTex = NULL;
Mix_Chunk* tickSound = NULL;
int mapWidth, mapHeight;
char infoText[128] = "";

unsigned char* collisionGrid = NULL;
int gridW, gridH;
GridPos* finalPath = NULL;
int pathLen = 0;

// --- Coordinate Helpers ---
int worldToScreenX(float wx) { return (int)((wx - camX) * zoom + WIDTH / 2); }
int worldToScreenY(float wy) { return (int)((wy - camY) * zoom + HEIGHT / 2 + TOPBAR); }
float screenToWorldX(int sx) { return (sx - WIDTH / 2) / zoom + camX; }
float screenToWorldY(int sy) { return (sy - TOPBAR - HEIGHT / 2) / zoom + camY; }

// Convert world coordinates to pixel coordinates of the image
float worldToPixelX(float wx) { return wx + mapWidth/2.0f; }
float worldToPixelY(float wy) { return wy + mapHeight/2.0f; }

// Convert pixel coordinates to latitude/longitude
float pixelToLat(float pixel_y) {
    return REF_LAT + (pixel_y - REF_PIXEL_Y) * LAT_PER_PIXEL;
}

float pixelToLon(float pixel_x) {
    return REF_LON + (pixel_x - REF_PIXEL_X) * LON_PER_PIXEL;
}

void wrapCamera() {
    float half = mapWidth * 0.5f;
    if (camX > half) camX -= mapWidth;
    else if (camX < -half) camX += mapWidth;
}

// --- Logic ---
void createCollisionGrid(SDL_Surface* surf) {
    gridW = surf->w / GRID_SCALE;
    gridH = surf->h / GRID_SCALE;
    collisionGrid = (unsigned char*)malloc(gridW * gridH);
    Uint32* pixels = (Uint32*)surf->pixels;
    for (int y = 0; y < gridH; y++) {
        for (int x = 0; x < gridW; x++) {
            Uint32 pixel = pixels[(y * GRID_SCALE * surf->w) + (x * GRID_SCALE)];
            Uint8 r, g, b;
            SDL_GetRGB(pixel, surf->format, &r, &g, &b);
            collisionGrid[y * gridW + x] = (r > 215 && g > 215 && b > 215) ? 0 : 1; 
        }
    }
    for (int y = 1; y < gridH - 1; y++) {
        for (int x = 1; x < gridW - 1; x++) {
            if (collisionGrid[y * gridW + x] == 0) {
                for (int dy = -1; dy <= 1; dy++) {
                    for (int dx = -1; dx <= 1; dx++) {
                        if (collisionGrid[(y + dy) * gridW + (x + dx)] == 1) {
                            collisionGrid[y * gridW + x] = 2;
                            break;
                        }
                    }
                }
            }
        }
    }
}

void snapToWater(Point* p) {
    int c = (int)(p->x + mapWidth/2) / GRID_SCALE;
    int r = (int)(p->y + mapHeight/2) / GRID_SCALE;
    if (r >= 0 && r < gridH && c >= 0 && c < gridW && collisionGrid[r * gridW + c] != 1) return;
    for (int radius = 1; radius < 25; radius++) {
        for (int dr = -radius; dr <= radius; dr++) {
            for (int dc = -radius; dc <= radius; dc++) {
                int nr = r + dr, nc = c + dc;
                if (nr >= 0 && nr < gridH && nc >= 0 && nc < gridW && collisionGrid[nr * gridW + nc] != 1) {
                    p->x = (nc * GRID_SCALE) - mapWidth/2.0f + (GRID_SCALE/2.0f);
                    p->y = (nr * GRID_SCALE) - mapHeight/2.0f + (GRID_SCALE/2.0f);
                    return;
                }
            }
        }
    }
}

void astar() {
    if (!p1.valid || !p2.valid) return;
    snapToWater(&p1); snapToWater(&p2);
    int startC = (int)(p1.x + mapWidth/2) / GRID_SCALE;
    int startR = (int)(p1.y + mapHeight/2) / GRID_SCALE;
    int endC = (int)(p2.x + mapWidth/2) / GRID_SCALE;
    int endR = (int)(p2.y + mapHeight/2) / GRID_SCALE;

    MinHeap openList = { malloc(sizeof(Node*) * gridW * gridH), 0 };
    Node* nodes = (Node*)calloc(gridW * gridH, sizeof(Node));
    float* gScore = (float*)malloc(gridW * gridH * sizeof(float));
    for(int i=0; i<gridW*gridH; i++) gScore[i] = 1e9f;

    int startIdx = startR * gridW + startC;
    gScore[startIdx] = 0;
    nodes[startIdx].pos = (GridPos){startR, startC};
    nodes[startIdx].f = sqrtf(pow(startR-endR, 2) + pow(startC-endC, 2));
    pushHeap(&openList, &nodes[startIdx]);

    int found = 0;
    while (openList.size > 0) {
        Node* curr = popHeap(&openList);
        if (curr->pos.r == endR && curr->pos.c == endC) {
            found = 1; pathLen = 0; Node* temp = curr;
            while(temp) { pathLen++; temp = temp->parent; }
            if(finalPath) free(finalPath);
            finalPath = malloc(sizeof(GridPos) * pathLen);
            temp = curr;
            for(int i=pathLen-1; i>=0; i--) { finalPath[i] = temp->pos; temp = temp->parent; }
            break;
        }

        for (int dr = -1; dr <= 1; dr++) {
            for (int dc = -1; dc <= 1; dc++) {
                if (dr == 0 && dc == 0) continue;
                int nr = curr->pos.r + dr, nc = curr->pos.c + dc;
                if (nr < 0 || nr >= gridH || nc < 0 || nc >= gridW || collisionGrid[nr * gridW + nc] == 1) continue;

                float stepCost = (dr == 0 || dc == 0) ? 1.0f : 1.414f;
                if (collisionGrid[nr * gridW + nc] == 2) stepCost += PADDING_COST; 

                float tentativeG = gScore[curr->pos.r * gridW + curr->pos.c] + stepCost;
                if (tentativeG < gScore[nr * gridW + nc]) {
                    int idx = nr * gridW + nc;
                    gScore[idx] = tentativeG;
                    nodes[idx].pos = (GridPos){nr, nc}; nodes[idx].parent = curr;
                    nodes[idx].g = tentativeG;
                    nodes[idx].h = sqrtf(pow(nr - endR, 2) + pow(nc - endC, 2)) * 1.2f;
                    nodes[idx].f = nodes[idx].g + nodes[idx].h;
                    pushHeap(&openList, &nodes[idx]);
                }
            }
        }
    }
    free(openList.nodes); free(nodes); free(gScore);
    snprintf(infoText, sizeof(infoText), found ? "Route Calculated" : "No Route Possible");
}

int main(int argc, char* argv[]) {
    SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO);
    TTF_Init(); IMG_Init(IMG_INIT_PNG);
    Mix_OpenAudio(22050, MIX_DEFAULT_FORMAT, 2, 512);

    SDL_Window* win = SDL_CreateWindow("Sea Route Planner", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, WIDTH, HEIGHT + TOPBAR, 0);
    SDL_Renderer* ren = SDL_CreateRenderer(win, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);

    tickSound = Mix_LoadWAV("assets/tick.wav");
    startTex = IMG_LoadTexture(ren, "assets/start.png");
    endTex = IMG_LoadTexture(ren, "assets/end.png");
    SDL_Surface* tempSurf = IMG_Load("assets/temp1.png");
    SDL_Surface* surf = SDL_ConvertSurfaceFormat(tempSurf, SDL_PIXELFORMAT_ARGB8888, 0);
    SDL_FreeSurface(tempSurf);
    mapWidth = surf->w; mapHeight = surf->h;
    createCollisionGrid(surf);
    mapTex = SDL_CreateTextureFromSurface(ren, surf);
    SDL_FreeSurface(surf);

    TTF_Font* font = TTF_OpenFont("arial.ttf", 16);
    TTF_Font* smallFont = TTF_OpenFont("arial.ttf", 12);
    SDL_Rect btnRect = {WIDTH - 160, 5, 150, 30};
    int dragging = 0, lastMouseX = 0, lastMouseY = 0, running = 1;
    Uint32 lastTicks = SDL_GetTicks();

    while (running) {
        Uint32 currentTicks = SDL_GetTicks();
        float deltaTime = (currentTicks - lastTicks) / 1000.0f;
        if (deltaTime > 0.1f) deltaTime = 0.016f; // Clamp delta
        lastTicks = currentTicks;

        SDL_Event e;
        while (SDL_PollEvent(&e)) {
            if (e.type == SDL_QUIT) running = 0;
            if (e.type == SDL_MOUSEWHEEL) {
                targetZoom += e.wheel.y * 0.12f;
                if (targetZoom < 0.2f) targetZoom = 0.2f;
                if(tickSound) Mix_PlayChannel(-1, tickSound, 0);
            }
            if (e.type == SDL_MOUSEBUTTONDOWN && e.button.button == SDL_BUTTON_LEFT) {
                if (e.button.y < TOPBAR && e.button.x >= btnRect.x) { astar(); }
                else if (e.button.y > TOPBAR) {
                    float wx = screenToWorldX(e.button.x), wy = screenToWorldY(e.button.y);
                    if (!p1.valid) { p1 = (Point){wx, wy, 1, 0}; }
                    else if (!p2.valid) { p2 = (Point){wx, wy, 1, 0}; }
                }
            }
            if (e.type == SDL_MOUSEBUTTONDOWN && e.button.button == SDL_BUTTON_RIGHT) { 
                dragging = 1; 
                lastMouseX = e.button.x; lastMouseY = e.button.y; 
                velX = velY = 0; 
                for(int i=0; i<VELOCITY_SAMPLES; i++) { frameVelX[i] = 0; frameVelY[i] = 0; }
            }
            if (e.type == SDL_MOUSEBUTTONUP && e.button.button == SDL_BUTTON_RIGHT) { 
                dragging = 0; 
                float avgX = 0, avgY = 0;
                for(int i = 0; i < VELOCITY_SAMPLES; i++){ avgX += frameVelX[i]; avgY += frameVelY[i]; }
                velX = avgX / VELOCITY_SAMPLES;
                velY = avgY / VELOCITY_SAMPLES;
            }
            if (e.type == SDL_MOUSEMOTION && dragging) {
                float dx = (e.motion.x - lastMouseX) / zoom;
                float dy = (e.motion.y - lastMouseY) / zoom;
                camX -= dx; camY -= dy;
                
                // Track velocity for inertia based on world-space change over time
                frameVelX[velIdx] = -dx / deltaTime;
                frameVelY[velIdx] = -dy / deltaTime;
                velIdx = (velIdx + 1) % VELOCITY_SAMPLES;

                lastMouseX = e.motion.x; lastMouseY = e.motion.y; wrapCamera();
            }
            if (e.type == SDL_KEYDOWN && e.key.keysym.sym == SDLK_c) { p1.valid = p2.valid = 0; pathLen = 0; }
        }

        // --- Animations & Physics ---
        zoom += (targetZoom - zoom) * 0.12f; // Easy ease zoom
        
        if (!dragging) {
            camX += velX * deltaTime;
            camY += velY * deltaTime;
            velX *= friction; // Slide to a stop
            velY *= friction;
            if (fabs(velX) < 1.0f) velX = 0;
            if (fabs(velY) < 1.0f) velY = 0;
            wrapCamera();
        }

        if (p1.valid && p1.alpha < 255) p1.alpha += 15;
        if (p2.valid && p2.alpha < 255) p2.alpha += 15;

        SDL_SetRenderDrawColor(ren, 255, 255, 255, 255);
        SDL_RenderClear(ren);

        for (int dx = -1; dx <= 1; dx++) {
            SDL_Rect dst = {worldToScreenX(-mapWidth/2 + dx*mapWidth), worldToScreenY(-mapHeight/2), (int)(mapWidth*zoom), (int)(mapHeight*zoom)};
            SDL_RenderCopy(ren, mapTex, NULL, &dst);
        }

        if (finalPath) {
            SDL_SetRenderDrawColor(ren, 0, 180, 255, 255);
            for (int i = 0; i < pathLen - 1; i++) {
                float wx1 = finalPath[i].c * GRID_SCALE - mapWidth/2.0f, wy1 = finalPath[i].r * GRID_SCALE - mapHeight/2.0f;
                float wx2 = finalPath[i+1].c * GRID_SCALE - mapWidth/2.0f, wy2 = finalPath[i+1].r * GRID_SCALE - mapHeight/2.0f;
                if (fabs(wx1 - wx2) < mapWidth / 2) SDL_RenderDrawLine(ren, worldToScreenX(wx1), worldToScreenY(wy1), worldToScreenX(wx2), worldToScreenY(wy2));
            }
        }

        // Draw points with Superscripts
        Point* pts[2] = {&p1, &p2};
        const char* labels[2] = {"A", "B"};
        SDL_Texture* icons[2] = {startTex, endTex};
        for(int i=0; i<2; i++) {
            if(pts[i]->valid) {
                SDL_SetTextureAlphaMod(icons[i], (Uint8)pts[i]->alpha);
                SDL_Rect r = {worldToScreenX(pts[i]->x)-12, worldToScreenY(pts[i]->y)-12, 25, 25};
                SDL_RenderCopy(ren, icons[i], NULL, &r);
                
                SDL_Surface* s = TTF_RenderText_Blended(smallFont, labels[i], (SDL_Color){255,0,0,255});
                SDL_Texture* t = SDL_CreateTextureFromSurface(ren, s);
                SDL_Rect tr = {worldToScreenX(pts[i]->x)+8, worldToScreenY(pts[i]->y)-20, s->w, s->h};
                SDL_RenderCopy(ren, t, NULL, &tr);
                SDL_FreeSurface(s); SDL_DestroyTexture(t);
            }
        }

        // --- UI Layer ---
        SDL_SetRenderDrawColor(ren, 240, 240, 240, 255);
        SDL_Rect bar = {0,0,WIDTH,TOPBAR}; SDL_RenderFillRect(ren, &bar);
        
        // Coordinates Display (Inside top bar - left side)
        if (p1.valid || p2.valid) {
            char coords[128];
            // Convert world coordinates to pixel coordinates, then to lat/lon
            float p1_pixel_x = worldToPixelX(p1.x);
            float p1_pixel_y = worldToPixelY(p1.y);
            float p2_pixel_x = worldToPixelX(p2.x);
            float p2_pixel_y = worldToPixelY(p2.y);
            
            float p1_lat = pixelToLat(p1_pixel_y);
            float p1_lon = pixelToLon(p1_pixel_x);
            float p2_lat = pixelToLat(p2_pixel_y);
            float p2_lon = pixelToLon(p2_pixel_x);
            
            snprintf(coords, sizeof(coords), "A: (%.6f, %.6f)  B: (%.6f, %.6f)", p1_lat, p1_lon, p2_lat, p2_lon);
            SDL_Surface* cs = TTF_RenderText_Blended(font, coords, (SDL_Color){50,50,50,255});
            SDL_Texture* ct = SDL_CreateTextureFromSurface(ren, cs);
            SDL_Rect cr = {10, 12, cs->w, cs->h};  // Moved to top bar at y=12 for vertical centering
            SDL_RenderCopy(ren, ct, NULL, &cr);
            SDL_FreeSurface(cs); SDL_DestroyTexture(ct);
        }

        SDL_SetRenderDrawColor(ren, 0, 150, 0, 255); SDL_RenderFillRect(ren, &btnRect);
        if (font) {
            SDL_Surface* s = TTF_RenderText_Blended(font, "COMPUTE PATH", (SDL_Color){255,255,255,255});
            SDL_Texture* t = SDL_CreateTextureFromSurface(ren, s);
            SDL_Rect tr = {btnRect.x+15, btnRect.y+7, s->w, s->h};
            SDL_RenderCopy(ren, t, NULL, &tr);
            SDL_FreeSurface(s); SDL_DestroyTexture(t);
        }
        SDL_RenderPresent(ren);
    }
    TTF_CloseFont(font); TTF_CloseFont(smallFont);
    SDL_Quit(); return 0;
}
