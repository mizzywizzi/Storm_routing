#define SDL_MAIN_HANDLED
#include <SDL2/SDL_mixer.h>
#include <SDL2/SDL.h>
#include <SDL2/SDL_ttf.h>
#include <SDL2/SDL_image.h>
#include <stdio.h>

#define WIDTH 1000
#define HEIGHT 700
#define TOPBAR 40

typedef struct { float x, y; int valid; } Point;

float zoom = 1.0f;
float camX = 0, camY = 0;

Point p1={0}, p2={0};
SDL_Texture* mapTex=NULL;
int mapWidth, mapHeight;
char infoText[128]="";

SDL_Texture* renderText(SDL_Renderer* r, TTF_Font* f, const char* t, SDL_Color c){
    SDL_Surface* s=TTF_RenderText_Blended(f,t,c);
    SDL_Texture* tex=SDL_CreateTextureFromSurface(r,s);
    SDL_FreeSurface(s);
    return tex;
}

int worldToScreenX(float wx){ return (int)((wx-camX)*zoom + WIDTH/2); }
int worldToScreenY(float wy){ return (int)((wy-camY)*zoom + HEIGHT/2 + TOPBAR); }

float screenToWorldX(int sx){ return (sx-WIDTH/2)/zoom + camX; }
float screenToWorldY(int sy){ return (sy-TOPBAR-HEIGHT/2)/zoom + camY; }

void drawGrid(SDL_Renderer* r){
    SDL_SetRenderDrawColor(r, 40,40,40,255); // darker grid
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

void drawPoint(SDL_Renderer* r, Point p){
    if(!p.valid) return;
    SDL_SetRenderDrawColor(r,255,0,0,255);

    // Wrap horizontally
    float px = p.x;
    while(px - camX > mapWidth/2) px -= mapWidth;
    while(px - camX < -mapWidth/2) px += mapWidth;

    SDL_Rect rect={worldToScreenX(px)-5, worldToScreenY(p.y)-5,10,10};
    SDL_RenderFillRect(r,&rect);
}

// Example affine conversion pixel->lat/lon
void pixelToGeo(double px,double py,double* lat,double* lon){
    *lat = -0.0002465*px -0.0572*py +113.5108;
    *lon =  0.05795*px +0.00001515*py -37.784;
}


int main(int argc,char* argv[]){
    SDL_SetMainReady();
    SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO);
    TTF_Init();
    IMG_Init(IMG_INIT_PNG);
    
    // Smaller buffer = lower latency (512 instead of 2048)
    Mix_OpenAudio(22050, MIX_DEFAULT_FORMAT, 2, 512);
    Mix_Init(MIX_INIT_MP3);

    SDL_Window* win=SDL_CreateWindow("Map Viewer",
        SDL_WINDOWPOS_CENTERED,SDL_WINDOWPOS_CENTERED,
        WIDTH,HEIGHT+TOPBAR,0);
    SDL_Renderer* ren=SDL_CreateRenderer(win,-1,SDL_RENDERER_ACCELERATED);
    
    Mix_Chunk* tickSound = Mix_LoadWAV("assets/tick.wav");
    if(tickSound){
        Mix_VolumeChunk(tickSound, MIX_MAX_VOLUME);
    }

    SDL_Surface* surf=IMG_Load("temp1.png");
    mapTex=SDL_CreateTextureFromSurface(ren,surf);
    mapWidth=surf->w; mapHeight=surf->h;
    SDL_FreeSurface(surf);

    TTF_Font* font=TTF_OpenFont("arial.ttf",16);
    SDL_Color black={0,0,0,255};

    int dragging=0,lastX,lastY,running=1;

    while(running){
        SDL_Event e;
        while(SDL_PollEvent(&e)){
            if(e.type==SDL_QUIT) running=0;

            if(e.type == SDL_MOUSEWHEEL){
                float oldZoom = zoom;

                zoom += e.wheel.y * 0.05f;

                if(zoom < 0.3f) zoom = 0.3f;   // min zoom out
                // (no max zoom limit)

                if(zoom != oldZoom && tickSound){   // only if zoom actually changed and sound loaded
                    int mx, my;
                    SDL_GetMouseState(&mx, &my);

                    float wx = screenToWorldX(mx);
                    float wy = screenToWorldY(my);

                    camX = wx - (mx - WIDTH/2) / zoom;
                    camY = wy - (my - TOPBAR - HEIGHT/2) / zoom;

                    Mix_PlayChannel(-1, tickSound, 0);
                }
            }

            if(e.type==SDL_MOUSEBUTTONDOWN && e.button.button==SDL_BUTTON_LEFT){
                float wx=screenToWorldX(e.button.x);
                float wy=screenToWorldY(e.button.y);

                if(!p1.valid){p1.x=wx;p1.y=wy;p1.valid=1;}
                else if(!p2.valid){p2.x=wx;p2.y=wy;p2.valid=1;}

                double px=wx+mapWidth/2;
                double py=wy+mapHeight/2;
                double lat,lon;
                pixelToGeo(px,py,&lat,&lon);
                snprintf(infoText,sizeof(infoText),"Pixel %.0f,%.0f | Lat %.6f Lon %.6f",px,py,lat,lon);
            }

            if(e.type==SDL_MOUSEBUTTONDOWN && e.button.button==SDL_BUTTON_RIGHT){
                dragging=1; lastX=e.button.x; lastY=e.button.y;
            }
            if(e.type==SDL_MOUSEBUTTONUP) dragging=0;

            if(e.type==SDL_MOUSEMOTION && dragging){
                camX-=(e.motion.x-lastX)/zoom;
                camY-=(e.motion.y-lastY)/zoom;
                lastX=e.motion.x; lastY=e.motion.y;

                // wrap horizontally
                if(camX>mapWidth/2) camX-=mapWidth;
                if(camX<-mapWidth/2) camX+=mapWidth;
            }

            if(e.type==SDL_KEYDOWN && e.key.keysym.sym==SDLK_c){p1.valid=p2.valid=0;}
        }

        SDL_SetRenderDrawColor(ren,255,255,255,255);
        SDL_RenderClear(ren);

        // DRAW HORIZONTALLY WRAPPING MAP (3 copies)
        for(int dx=-1;dx<=1;dx++){
            SDL_Rect dst={worldToScreenX(-mapWidth/2 + dx*mapWidth), worldToScreenY(-mapHeight/2),
                          mapWidth*zoom, mapHeight*zoom};
            SDL_RenderCopy(ren,mapTex,NULL,&dst);
        }

        // DRAW GRID AND POINTS ABOVE MAP
        drawGrid(ren);
        drawPoint(ren,p1);
        drawPoint(ren,p2);

        // DRAW TOP BAR LAST WITH DROP SHADOW
        SDL_SetRenderDrawColor(ren,0,0,0,100); // shadow
        SDL_Rect shadow={0,TOPBAR,WIDTH,4};
        SDL_RenderFillRect(ren,&shadow);

        SDL_SetRenderDrawColor(ren,255,255,255,255); // white bar
        SDL_Rect topBar={0,0,WIDTH,TOPBAR};
        SDL_RenderFillRect(ren,&topBar);

        // Zoom text
        char ztxt[32];
        snprintf(ztxt,sizeof(ztxt),"Zoom: %.2f",zoom);
        SDL_Texture* zt=renderText(ren,font,ztxt,black);
        SDL_Rect zr={10,8,0,0};
        SDL_QueryTexture(zt,NULL,NULL,&zr.w,&zr.h);
        SDL_RenderCopy(ren,zt,NULL,&zr);
        SDL_DestroyTexture(zt);

        // Info text
        if(infoText[0]){
            SDL_Texture* it=renderText(ren,font,infoText,black);
            SDL_Rect ir={200,8,0,0};
            SDL_QueryTexture(it,NULL,NULL,&ir.w,&ir.h);
            SDL_RenderCopy(ren,it,NULL,&ir);
            SDL_DestroyTexture(it);
        }

        SDL_RenderPresent(ren);
        SDL_Delay(16);
    }

    // Cleanup
    if(tickSound) Mix_FreeChunk(tickSound);
    SDL_DestroyTexture(mapTex);
    TTF_CloseFont(font);
    SDL_DestroyRenderer(ren);
    SDL_DestroyWindow(win);
    Mix_CloseAudio();
    Mix_Quit();
    IMG_Quit();
    TTF_Quit();
    SDL_Quit();
    
    return 0;
}
