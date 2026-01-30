#define SDL_MAIN_HANDLED
#include <SDL2/SDL_mixer.h>
#include <SDL2/SDL.h>
#include <SDL2/SDL_ttf.h>
#include <SDL2/SDL_image.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#define WIDTH 1000
#define HEIGHT 700
#define TOPBAR 40
#define VELOCITY_SAMPLES 5
#define MAX_PATH_POINTS 10000

typedef struct { float x, y; int valid; } Point;

typedef struct {
    int gridW, gridH;
    int blockSize;
    int left, right, top, bottom;
    int padding;
    char** cells; // 0 = water, 1 = land
    int startGX, startGY;
    int endGX, endGY;
    float fadeAlpha;
    int* lineGridsX;  // Grid cells the line passes through
    int* lineGridsY;
    int lineGridCount;
} PathGrid;

float zoom = 1.0f;
float camX = 0, camY = 0;

// Smooth pan animation variables
float velX = 0.0f, velY = 0.0f;
float friction = 0.94f;
float accel = 2500.0f;
float frameVelX[VELOCITY_SAMPLES] = {0};
float frameVelY[VELOCITY_SAMPLES] = {0};
int velIdx = 0;

Point p1={0}, p2={0};
double p1_lat = 0, p1_lon = 0, p2_lat = 0, p2_lon = 0;
SDL_Texture* mapTex=NULL;
SDL_Surface* mapSurf=NULL;
int mapWidth, mapHeight;
char infoText[128]="";

/* Land polygons (optional binary file; stub if not used) */
typedef struct { float* points; int pointCount; } LandPolygon;
LandPolygon* landPolygons = NULL;
int polygonCount = 0;

/* Pathfinding */
PathGrid grid = {0};
int computingPath = 0;
int gridReady = 0;

#define GRID_SIZE_MIN 4
#define GRID_SIZE_MAX 64
int gridBlockSize = 16;  /* default grid cell size in pixels; user can change via slider */

int loadLandPolygons(const char* path) {
    (void)path;
    landPolygons = NULL;
    polygonCount = 0;
    return 1;  /* stub: no file loaded, no error */
}

SDL_Texture* renderText(SDL_Renderer* r, TTF_Font* f, const char* t, SDL_Color c){
    SDL_Surface* s=TTF_RenderText_Blended(f,t,c);
    if(!s) return NULL;
    SDL_Texture* tex=SDL_CreateTextureFromSurface(r,s);
    SDL_FreeSurface(s);
    return tex;
}

int worldToScreenX(float wx){ return (int)((wx-camX)*zoom + WIDTH/2); }
int worldToScreenY(float wy){ return (int)((wy-camY)*zoom + HEIGHT/2 + TOPBAR); }

float screenToWorldX(int sx){ return (sx-WIDTH/2)/zoom + camX; }
float screenToWorldY(int sy){ return (sy-TOPBAR-HEIGHT/2)/zoom + camY; }

void wrapCamera() {
    float half = mapWidth * 0.5f;
    if (camX > half) camX -= mapWidth;
    else if (camX < -half) camX += mapWidth;
}

// Bresenham's line algorithm to get all grid cells a line passes through
void getLineGridCells(int x0, int y0, int x1, int y1, int** outX, int** outY, int* count) {
    int dx = abs(x1 - x0);
    int dy = abs(y1 - y0);
    int sx = x0 < x1 ? 1 : -1;
    int sy = y0 < y1 ? 1 : -1;
    int err = dx - dy;

    int capacity = dx + dy + 10;
    *outX = (int*)malloc(capacity * sizeof(int));
    *outY = (int*)malloc(capacity * sizeof(int));
    *count = 0;

    int lastGX = -999, lastGY = -999;

    while (1) {
        if (x0 != lastGX || y0 != lastGY) {
            (*outX)[*count] = x0;
            (*outY)[*count] = y0;
            (*count)++;
            lastGX = x0;
            lastGY = y0;
        }
        if (x0 == x1 && y0 == y1) break;

        int e2 = 2 * err;
        if (e2 > -dy) { err -= dy; x0 += sx; }
        if (e2 < dx) { err += dx; y0 += sy; }
    }
}

// Check if grid cell is land or water using color sampling
int isLandByColor(int worldX, int worldY) {
    if (!mapSurf) return 0;

    int px = worldX + mapWidth/2;
    int py = worldY + mapHeight/2;

    if(px < 0 || px >= mapWidth || py < 0 || py >= mapHeight) return 0;

    /* Use pitch for row stride (surface may have padding) */
    int bpp = mapSurf->format->BytesPerPixel;
    Uint8* row = (Uint8*)mapSurf->pixels + (size_t)py * mapSurf->pitch;
    Uint32 pixel = 0;
    memcpy(&pixel, row + (size_t)px * bpp, (size_t)bpp);

    Uint8 r, g, b;
    SDL_GetRGB(pixel, mapSurf->format, &r, &g, &b);

    /* 255,255,255 = water; all other = land */
    if(r == 255 && g == 255 && b == 255) return 0; // water
    return 1; // land
}

void initGrid(int blockSize) {
    if (grid.cells) {
        for (int i = 0; i < grid.gridH; i++) free(grid.cells[i]);
        free(grid.cells);
    }
    if (grid.lineGridsX) free(grid.lineGridsX);
    if (grid.lineGridsY) free(grid.lineGridsY);

    grid.blockSize = blockSize;
    grid.padding = 300;

    grid.left = (int)fmin(p1.x, p2.x) - grid.padding;
    grid.right = (int)fmax(p1.x, p2.x) + grid.padding;
    grid.top = (int)fmin(p1.y, p2.y) - grid.padding;
    grid.bottom = (int)fmax(p1.y, p2.y) + grid.padding;

    grid.gridW = (grid.right - grid.left) / blockSize;
    grid.gridH = (grid.bottom - grid.top) / blockSize;

    grid.cells = (char**)malloc(grid.gridH * sizeof(char*));
    for (int i = 0; i < grid.gridH; i++)
        grid.cells[i] = (char*)calloc(grid.gridW, sizeof(char));

    grid.startGX = ((int)p1.x - grid.left) / blockSize;
    grid.startGY = ((int)p1.y - grid.top) / blockSize;
    grid.endGX = ((int)p2.x - grid.left) / blockSize;
    grid.endGY = ((int)p2.y - grid.top) / blockSize;

    getLineGridCells(grid.startGX, grid.startGY, grid.endGX, grid.endGY,
                     &grid.lineGridsX, &grid.lineGridsY, &grid.lineGridCount);

    int landCellsFound = 0;
    printf("--- Grid cells: (gx,gy) world (worldX,worldY) pixel (px,py) color (R,G,B) type ---\n");
    for (int i = 0; i < grid.lineGridCount; i++) {
        int gx = grid.lineGridsX[i];
        int gy = grid.lineGridsY[i];

        if (gx < 0 || gx >= grid.gridW || gy < 0 || gy >= grid.gridH) continue;

        int worldX = grid.left + gx * blockSize + blockSize / 2;
        int worldY = grid.top + gy * blockSize + blockSize / 2;
        int px = worldX + mapWidth/2;
        int py = worldY + mapHeight/2;

        Uint8 r = 0, g = 0, b = 0;
        if (mapSurf && px >= 0 && px < mapWidth && py >= 0 && py < mapHeight) {
            int bpp = mapSurf->format->BytesPerPixel;
            Uint8* row = (Uint8*)mapSurf->pixels + (size_t)py * mapSurf->pitch;
            Uint32 pixel = 0;
            memcpy(&pixel, row + (size_t)px * bpp, (size_t)bpp);
            SDL_GetRGB(pixel, mapSurf->format, &r, &g, &b);
        }

        /* 255,255,255 = water; all other = land */
        int land = (r == 255 && g == 255 && b == 255) ? 0 : 1;
        grid.cells[gy][gx] = land;
        if (land) landCellsFound++;

        printf("grid (%d,%d) world (%d,%d) pixel (%d,%d) color (%u,%u,%u) %s\n",
               gx, gy, worldX, worldY, px, py, (unsigned)r, (unsigned)g, (unsigned)b,
               land ? "land" : "water");
    }
    printf("--- end grid cells ---\n");

    if (landCellsFound > 0)
        snprintf(infoText, sizeof(infoText), "Path blocked by %d land cells", landCellsFound);
    else
        snprintf(infoText, sizeof(infoText), "Direct path clear! %d water cells", grid.lineGridCount);

    grid.fadeAlpha = 0;
    gridReady = 1;
}

// The rest of your drawGrid, drawPathGrid, drawPoint, drawButton, main loop
// remains the same, just remove the polygon references and pointInPolygon stuff.



void drawGrid(SDL_Renderer* r){
    SDL_SetRenderDrawColor(r, 40,40,40,255);
    int step=100;

    int left=(int)screenToWorldX(0);
    int right=(int)screenToWorldX(WIDTH);
    int top=(int)screenToWorldY(TOPBAR);
    int bottom=(int)screenToWorldY(HEIGHT+TOPBAR);

    for(int x=(left/step)*step; x<=right; x+=step){
        int sx = worldToScreenX(x);
        SDL_RenderDrawLine(r, sx, TOPBAR, sx, HEIGHT+TOPBAR);
    }

    for(int y=(top/step)*step; y<=bottom; y+=step){
        int sy = worldToScreenY(y);
        SDL_RenderDrawLine(r, 0, sy, WIDTH, sy);
    }
}

void drawPathGrid(SDL_Renderer* r) {
    if (!gridReady) return;
    
    // Fade in effect
    if (grid.fadeAlpha < 255) {
        grid.fadeAlpha += 5;
        if (grid.fadeAlpha > 255) grid.fadeAlpha = 255;
    }
    
    SDL_SetRenderDrawBlendMode(r, SDL_BLENDMODE_BLEND);
    
    // Draw only cells along the line
    for (int i = 0; i < grid.lineGridCount; i++) {
        int gx = grid.lineGridsX[i];
        int gy = grid.lineGridsY[i];
        
        if (gx < 0 || gx >= grid.gridW || gy < 0 || gy >= grid.gridH) continue;
        
        float worldX = grid.left + gx * grid.blockSize;
        float worldY = grid.top + gy * grid.blockSize;
        
        int screenX = worldToScreenX(worldX);
        int screenY = worldToScreenY(worldY);
        int cellSize = (int)(grid.blockSize * zoom);
        
        // Skip if outside screen
        if (screenX + cellSize < 0 || screenX > WIDTH || 
            screenY + cellSize < TOPBAR || screenY > HEIGHT + TOPBAR) {
            continue;
        }
        
        SDL_Rect cellRect = {screenX, screenY, cellSize, cellSize};
        
        // Color based on cell type
        if (gx == grid.startGX && gy == grid.startGY) {
            // Start cell (bright green)
            SDL_SetRenderDrawColor(r, 0, 255, 0, (int)(grid.fadeAlpha * 0.7));
            SDL_RenderFillRect(r, &cellRect);
        } else if (gx == grid.endGX && gy == grid.endGY) {
            // End cell (bright blue)
            SDL_SetRenderDrawColor(r, 0, 100, 255, (int)(grid.fadeAlpha * 0.7));
            SDL_RenderFillRect(r, &cellRect);
        } else if (grid.cells[gy][gx] == 1) {
            // Land (RED - blocking!)
            SDL_SetRenderDrawColor(r, 255, 50, 50, (int)(grid.fadeAlpha * 0.6));
            SDL_RenderFillRect(r, &cellRect);
        } else {
            // Water (green - clear path)
            SDL_SetRenderDrawColor(r, 50, 255, 150, (int)(grid.fadeAlpha * 0.4));
            SDL_RenderFillRect(r, &cellRect);
        }
        
        // Draw grid lines
        SDL_SetRenderDrawColor(r, 255, 255, 255, (int)(grid.fadeAlpha * 0.5));
        SDL_RenderDrawRect(r, &cellRect);
    }
    
    // Draw the straight line path
    if (grid.lineGridCount > 0) {
        SDL_SetRenderDrawColor(r, 255, 255, 0, (int)(grid.fadeAlpha * 0.8));
        for (int i = 0; i < grid.lineGridCount - 1; i++) {
            int gx1 = grid.lineGridsX[i];
            int gy1 = grid.lineGridsY[i];
            int gx2 = grid.lineGridsX[i + 1];
            int gy2 = grid.lineGridsY[i + 1];
            
            float wx1 = grid.left + gx1 * grid.blockSize + grid.blockSize / 2;
            float wy1 = grid.top + gy1 * grid.blockSize + grid.blockSize / 2;
            float wx2 = grid.left + gx2 * grid.blockSize + grid.blockSize / 2;
            float wy2 = grid.top + gy2 * grid.blockSize + grid.blockSize / 2;
            
            int sx1 = worldToScreenX(wx1);
            int sy1 = worldToScreenY(wy1);
            int sx2 = worldToScreenX(wx2);
            int sy2 = worldToScreenY(wy2);
            
            SDL_RenderDrawLine(r, sx1, sy1, sx2, sy2);
        }
    }
}

void drawPoint(SDL_Renderer* r, TTF_Font* font, Point p, const char* label, double lat, double lon){
    if(!p.valid) return;
    
    // Draw point
    SDL_SetRenderDrawColor(r,255,0,0,255);

    float px = p.x;
    while(px - camX > mapWidth/2) px -= mapWidth;
    while(px - camX < -mapWidth/2) px += mapWidth;

    int screenX = worldToScreenX(px);
    int screenY = worldToScreenY(p.y);
    
    SDL_Rect rect={screenX-5, screenY-5, 10, 10};
    SDL_RenderFillRect(r, &rect);
    
    // Draw label on map: A:(latitude,longitude) (pixelX,pixelY)
    int pixX = (int)(p.x + mapWidth/2);
    int pixY = (int)(p.y + mapHeight/2);
    char labelText[128];
    snprintf(labelText, sizeof(labelText), "%s:(%.6f,%.6f) (%d,%d)", label, lat, lon, pixX, pixY);
    
    SDL_Color white = {255, 255, 255, 255};
    
    SDL_Texture* textTex = renderText(r, font, labelText, white);
    if(!textTex) return;
    
    int textW, textH;
    SDL_QueryTexture(textTex, NULL, NULL, &textW, &textH);
    
    // Position label above and to the right of point
    SDL_Rect textRect = {screenX + 10, screenY - 25, textW, textH};
    SDL_Rect bgRect = {textRect.x - 4, textRect.y - 2, textRect.w + 8, textRect.h + 4};
    
    // Draw background
    SDL_SetRenderDrawBlendMode(r, SDL_BLENDMODE_BLEND);
    SDL_SetRenderDrawColor(r, 0, 0, 0, 200);
    SDL_RenderFillRect(r, &bgRect);
    
    // Draw border
    SDL_SetRenderDrawColor(r, 255, 0, 0, 255);
    SDL_RenderDrawRect(r, &bgRect);
    
    // Draw text
    SDL_RenderCopy(r, textTex, NULL, &textRect);
    SDL_DestroyTexture(textTex);
}

void drawButton(SDL_Renderer* r, TTF_Font* font, const char* text, int x, int y, int* w, int* h, int hover) {
    SDL_Color textColor = {255, 255, 255, 255};
    SDL_Texture* textTex = renderText(r, font, text, textColor);
    if (!textTex) return;
    
    int textW, textH;
    SDL_QueryTexture(textTex, NULL, NULL, &textW, &textH);
    
    int padding = 15;
    int btnW = textW + padding * 2;
    int btnH = textH + padding;
    
    SDL_Rect btnRect = {x, y, btnW, btnH};
    SDL_Rect textRect = {x + padding, y + padding / 2, textW, textH};
    
    // Button background
    SDL_SetRenderDrawBlendMode(r, SDL_BLENDMODE_BLEND);
    if (hover) {
        SDL_SetRenderDrawColor(r, 0, 120, 215, 255);
    } else {
        SDL_SetRenderDrawColor(r, 0, 100, 180, 255);
    }
    SDL_RenderFillRect(r, &btnRect);
    
    // Button border
    SDL_SetRenderDrawColor(r, 255, 255, 255, 200);
    SDL_RenderDrawRect(r, &btnRect);
    
    // Text
    SDL_RenderCopy(r, textTex, NULL, &textRect);
    SDL_DestroyTexture(textTex);
    
    if (w) *w = btnW;
    if (h) *h = btnH;
}

int isMouseOverButton(int mx, int my, int x, int y, int w, int h) {
    return mx >= x && mx <= x + w && my >= y && my <= y + h;
}

int isMouseOverSlider(int mx, int my, int x, int y, int w, int h) {
    return mx >= x && mx <= x + w && my >= y && my <= y + h;
}

void drawSlider(SDL_Renderer* r, TTF_Font* font, int x, int y, int w, int h,
                int value, int minVal, int maxVal, SDL_Color textColor) {
    const int thumbW = 14;
    int range = maxVal - minVal;
    if (range <= 0) range = 1;
    int thumbX = x + (value - minVal) * (w - thumbW) / range;
    if (thumbX < x) thumbX = x;
    if (thumbX > x + w - thumbW) thumbX = x + w - thumbW;

    /* Track background */
    SDL_SetRenderDrawColor(r, 60, 60, 60, 255);
    SDL_Rect track = { x, y + (h - 6) / 2, w, 6 };
    SDL_RenderFillRect(r, &track);

    /* Thumb */
    SDL_SetRenderDrawColor(r, 0, 120, 215, 255);
    SDL_Rect thumb = { thumbX, y, thumbW, h };
    SDL_RenderFillRect(r, &thumb);
    SDL_SetRenderDrawColor(r, 255, 255, 255, 200);
    SDL_RenderDrawRect(r, &thumb);

    /* Label "Grid: N" */
    char label[32];
    snprintf(label, sizeof(label), "Grid: %d", value);
    SDL_Texture* tex = renderText(r, font, label, textColor);
    if (tex) {
        int tw, th;
        SDL_QueryTexture(tex, NULL, NULL, &tw, &th);
        SDL_Rect labelRect = { x + w + 8, y + (h - th) / 2, tw, th };
        SDL_RenderCopy(r, tex, NULL, &labelRect);
        SDL_DestroyTexture(tex);
    }
}

void pixelToGeo(double px,double py,double* lat,double* lon){
    *lat = -0.0002465*px -0.0572*py +113.5108;
    *lon =  0.05795*px +0.00001515*py -37.784;
}

int main(int argc,char* argv[]){
    SDL_SetMainReady();
    SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO);
    TTF_Init();
    IMG_Init(IMG_INIT_PNG);
    
    Mix_OpenAudio(22050, MIX_DEFAULT_FORMAT, 2, 512);
    Mix_Init(MIX_INIT_MP3);

    SDL_Window* win=SDL_CreateWindow("Map Viewer - Smart Pathfinding",
        SDL_WINDOWPOS_CENTERED,SDL_WINDOWPOS_CENTERED,
        WIDTH,HEIGHT+TOPBAR,0);
    SDL_Renderer* ren=SDL_CreateRenderer(win,-1,SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
    
    Mix_Chunk* tickSound = Mix_LoadWAV("assets/tick.wav");
    if(tickSound){
        Mix_VolumeChunk(tickSound, MIX_MAX_VOLUME);
    }

    SDL_Surface* surf=IMG_Load("assets/temp1.png");
    if (!surf) {
        printf("Failed to load map image!\n");
        return 1;
    }
    mapSurf = surf;  /* keep surface for pixel sampling (color / land-water) */
    mapTex=SDL_CreateTextureFromSurface(ren,surf);
    mapWidth=surf->w; 
    mapHeight=surf->h;
    
    printf("Map loaded: %dx%d\n", mapWidth, mapHeight);
    
    // Load land polygons
    if (!loadLandPolygons("assets/land_polygons.bin")) {
        printf("Warning: Could not load land polygons, all cells will be water\n");
    }

    TTF_Font* font=TTF_OpenFont("arial.ttf",16);
    if(!font){
        font=TTF_OpenFont("/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",16);
    }
    SDL_Color black={0,0,0,255};

    int dragging=0, lastMouseX=0, lastMouseY=0, running=1;
    int btnW = 0, btnH = 0;
    int btnX = WIDTH - 180, btnY = 50;
    int sliderX = 10, sliderY = 50, sliderW = 140, sliderH = 20;
    int sliderDragging = 0;
    
    // Delta time tracking
    Uint32 lastTicks = SDL_GetTicks();

    while(running){
        // Calculate delta time
        Uint32 currentTicks = SDL_GetTicks();
        float deltaTime = (currentTicks - lastTicks) / 1000.0f;
        if (deltaTime > 0.1f) deltaTime = 0.016f;
        lastTicks = currentTicks;

        int mx, my;
        SDL_GetMouseState(&mx, &my);
        int btnHover = p1.valid && p2.valid && isMouseOverButton(mx, my, btnX, btnY, btnW, btnH);

        SDL_Event e;
        while(SDL_PollEvent(&e)){
            if(e.type==SDL_QUIT) running=0;

            if(e.type == SDL_MOUSEWHEEL){
                float oldZoom = zoom;

                // Get world position before zoom
                float wx = screenToWorldX(mx);
                float wy = screenToWorldY(my);

                zoom += e.wheel.y * 0.05f;
                if(zoom < 0.3f) zoom = 0.3f;

                if(zoom != oldZoom){
                    // Adjust camera to keep mouse position fixed
                    camX = wx - (mx - WIDTH/2) / zoom;
                    camY = wy - (my - TOPBAR - HEIGHT/2) / zoom;
                    wrapCamera();

                    if(tickSound) Mix_PlayChannel(-1, tickSound, 0);
                }
            }

            if(e.type==SDL_MOUSEBUTTONDOWN && e.button.button==SDL_BUTTON_LEFT){
                // Check if clicking compute button
                if (p1.valid && p2.valid && btnHover) {
                    computingPath = 1;
                    initGrid(gridBlockSize);
                    continue;
                }
                // Check if clicking slider
                if (isMouseOverSlider(e.button.x, e.button.y, sliderX, sliderY, sliderW, sliderH)) {
                    sliderDragging = 1;
                    /* set grid size from mouse position */
                    int v = GRID_SIZE_MIN + (e.button.x - sliderX) * (GRID_SIZE_MAX - GRID_SIZE_MIN) / sliderW;
                    if (v < GRID_SIZE_MIN) v = GRID_SIZE_MIN;
                    if (v > GRID_SIZE_MAX) v = GRID_SIZE_MAX;
                    gridBlockSize = v;
                    continue;
                }
                
                float wx=screenToWorldX(e.button.x);
                float wy=screenToWorldY(e.button.y);

                double px=wx+mapWidth/2;
                double py=wy+mapHeight/2;
                double lat,lon;
                pixelToGeo(px,py,&lat,&lon);

                if(!p1.valid){
                    p1.x=wx;
                    p1.y=wy;
                    p1.valid=1;
                    p1_lat = lat;
                    p1_lon = lon;
                    snprintf(infoText,sizeof(infoText),"Point A placed at (%.6f, %.6f)",lat,lon);
                }
                else if(!p2.valid){
                    p2.x=wx;
                    p2.y=wy;
                    p2.valid=1;
                    p2_lat = lat;
                    p2_lon = lon;
                    snprintf(infoText,sizeof(infoText),"Point B placed at (%.6f, %.6f)",lat,lon);
                }
            }

            if(e.type==SDL_MOUSEBUTTONDOWN && e.button.button==SDL_BUTTON_RIGHT){
                dragging=1;
                velX = 0; velY = 0;
                for(int i = 0; i < VELOCITY_SAMPLES; i++){
                    frameVelX[i] = 0;
                    frameVelY[i] = 0;
                }
                lastMouseX=e.button.x;
                lastMouseY=e.button.y;
            }

            if(e.type==SDL_MOUSEBUTTONUP && e.button.button==SDL_BUTTON_LEFT){
                sliderDragging = 0;
            }
            if(e.type==SDL_MOUSEBUTTONUP && e.button.button==SDL_BUTTON_RIGHT){
                dragging=0;
                // Apply momentum from drag
                float avgX = 0, avgY = 0;
                for(int i = 0; i < VELOCITY_SAMPLES; i++){
                    avgX += frameVelX[i];
                    avgY += frameVelY[i];
                }
                velX = avgX / VELOCITY_SAMPLES;
                velY = avgY / VELOCITY_SAMPLES;
            }

            if(e.type==SDL_MOUSEMOTION && sliderDragging){
                int v = GRID_SIZE_MIN + (e.motion.x - sliderX) * (GRID_SIZE_MAX - GRID_SIZE_MIN) / sliderW;
                if (v < GRID_SIZE_MIN) v = GRID_SIZE_MIN;
                if (v > GRID_SIZE_MAX) v = GRID_SIZE_MAX;
                gridBlockSize = v;
            }
            if(e.type==SDL_MOUSEMOTION && dragging){
                float dx = (e.motion.x - lastMouseX) / zoom;
                float dy = (e.motion.y - lastMouseY) / zoom;
                
                camX -= dx;
                camY -= dy;
                
                // Track velocity for momentum
                frameVelX[velIdx] = -dx / deltaTime;
                frameVelY[velIdx] = -dy / deltaTime;
                velIdx = (velIdx + 1) % VELOCITY_SAMPLES;
                
                wrapCamera();
                lastMouseX = e.motion.x;
                lastMouseY = e.motion.y;
            }

            if(e.type==SDL_KEYDOWN && e.key.keysym.sym==SDLK_c){
                p1.valid=p2.valid=0;
                gridReady = 0;
                computingPath = 0;
                infoText[0] = '\0';
            }
        }

        // Keyboard acceleration (when not dragging)
        const Uint8* state = SDL_GetKeyboardState(NULL);
        if (!dragging) {
            float cA = accel / zoom;
            if (state[SDL_SCANCODE_W] || state[SDL_SCANCODE_UP])    velY += cA * deltaTime;
            if (state[SDL_SCANCODE_S] || state[SDL_SCANCODE_DOWN])  velY -= cA * deltaTime;
            if (state[SDL_SCANCODE_A] || state[SDL_SCANCODE_LEFT])  velX += cA * deltaTime;
            if (state[SDL_SCANCODE_D] || state[SDL_SCANCODE_RIGHT]) velX -= cA * deltaTime;
        }

        // Apply velocity
        camX += velX * deltaTime;
        camY += velY * deltaTime;
        
        // Apply friction
        velX *= friction;
        velY *= friction;
        
        // Stop very small velocities
        if (fabs(velX) < 1.0f / zoom) velX = 0;
        if (fabs(velY) < 1.0f / zoom) velY = 0;
        
        wrapCamera();

        SDL_SetRenderDrawColor(ren,255,255,255,255);
        SDL_RenderClear(ren);

        // Draw wrapping map
        for(int dx=-1;dx<=1;dx++){
            SDL_Rect dst={worldToScreenX(-mapWidth/2 + dx*mapWidth), worldToScreenY(-mapHeight/2),
                          mapWidth*zoom, mapHeight*zoom};
            SDL_RenderCopy(ren,mapTex,NULL,&dst);
        }

        drawGrid(ren);
        
        // Draw pathfinding grid if ready
        if (gridReady) {
            drawPathGrid(ren);
        }
        
        // Draw points with labels
        drawPoint(ren, font, p1, "A", p1_lat, p1_lon);
        drawPoint(ren, font, p2, "B", p2_lat, p2_lon);

        // Top bar with shadow
        SDL_SetRenderDrawColor(ren,0,0,0,100);
        SDL_Rect shadow={0,TOPBAR,WIDTH,4};
        SDL_RenderFillRect(ren,&shadow);

        SDL_SetRenderDrawColor(ren,255,255,255,255);
        SDL_Rect topBar={0,0,WIDTH,TOPBAR};
        SDL_RenderFillRect(ren,&topBar);

        // Zoom text
        char ztxt[32];
        snprintf(ztxt,sizeof(ztxt),"Zoom: %.2f",zoom);
        SDL_Texture* zt=renderText(ren,font,ztxt,black);
        if(zt){
            SDL_Rect zr={10,8,0,0};
            SDL_QueryTexture(zt,NULL,NULL,&zr.w,&zr.h);
            SDL_RenderCopy(ren,zt,NULL,&zr);
            SDL_DestroyTexture(zt);
        }

        // Info text
        if(infoText[0]){
            SDL_Texture* it=renderText(ren,font,infoText,black);
            if(it){
                SDL_Rect ir={200,8,0,0};
                SDL_QueryTexture(it,NULL,NULL,&ir.w,&ir.h);
                SDL_RenderCopy(ren,it,NULL,&ir);
                SDL_DestroyTexture(it);
            }
        }
        
        // Draw grid size slider (left side, same row as button)
        drawSlider(ren, font, sliderX, sliderY, sliderW, sliderH,
                   gridBlockSize, GRID_SIZE_MIN, GRID_SIZE_MAX, black);

        // Draw "Compute Path" button if both points are placed
        if (p1.valid && p2.valid) {
            drawButton(ren, font, "Compute Path", btnX, btnY, &btnW, &btnH, btnHover);
        }

        SDL_RenderPresent(ren);
    }

    // Cleanup
    if (landPolygons) {
        for (int i = 0; i < polygonCount; i++) {
            free(landPolygons[i].points);
        }
        free(landPolygons);
    }
    if (grid.cells) {
        for (int i = 0; i < grid.gridH; i++) {
            free(grid.cells[i]);
        }
        free(grid.cells);
    }
    if (grid.lineGridsX) free(grid.lineGridsX);
    if (grid.lineGridsY) free(grid.lineGridsY);
    
    if (mapSurf) { SDL_FreeSurface(mapSurf); mapSurf = NULL; }
    if(tickSound) Mix_FreeChunk(tickSound);
    SDL_DestroyTexture(mapTex);
    if(font) TTF_CloseFont(font);
    SDL_DestroyRenderer(ren);
    SDL_DestroyWindow(win);
    Mix_CloseAudio();
    Mix_Quit();
    IMG_Quit();
    TTF_Quit();
    SDL_Quit();
    
    return 0;
}
