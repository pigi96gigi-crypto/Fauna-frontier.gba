
/*
    FaunaFrontierGBA — Enhanced (testuale) per Game Boy Advance
    - libgba console su BG0
    - Mappa 80x64 con biome (Prato, Bosco, Deserto, Acqua, Monti)
    - Missioni, Base building, Cattura, Minimappa, NPC/Dialoghi, SRAM Save, Boss notturno
    Licenza: MIT - completamente originale
*/

#include <gba_base.h>
#include <gba_console.h>
#include <gba_interrupt.h>
#include <gba_input.h>
#include <gba_systemcalls.h>
#include <gba_video.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAP_W 80
#define MAP_H 64
#define VIEW_W 30
#define VIEW_H 20

// Tiles (caratteri)
#define TILE_EMPTY '.'
#define TILE_GRASS 'G'
#define TILE_TREE  'Y'
#define TILE_WALL  '#'
#define TILE_WATER 'W'
#define TILE_SAND  'S'
#define TILE_BASE  '='
#define TILE_TOWER 'T'
#define TILE_FARM  'F'
#define TILE_FIRE  'H'
#define TILE_POST  'P'
#define TILE_NPC   '@'

#define MAX_COMPANIONS 3
#define MAX_MISSIONS   5
#define MAX_NPC        6

typedef enum { GS_WORLD=0, GS_BATTLE=1, GS_MSG=2, GS_MENU=3, GS_BOSS=4 } GameState;
typedef enum { TYPE_NEUTRAL=0, TYPE_FIRE, TYPE_WATER, TYPE_GRASS, TYPE_ELEC } ElemType;

typedef struct {
    const char* name;
    ElemType type;
    int max_hp;
    int hp;
    int atk;
    int speed;
    const char* ability;
    int caught;
} Creature;

typedef struct {
    int x, y;
    int steps;
    int orbs;
    int wood;
    int stone;
} Player;

typedef struct {
    const char* name;
    int required_wood;
    int required_stone;
    char tile_char;
} BuildDef;

typedef struct {
    const char* title;
    const char* desc;
    int completed;
} Mission;

typedef struct {
    int x, y;
    const char* name;
    const char* lines[3];
    int line_count;
    int gave_gift;
    int gift_wood;
    int gift_stone;
    int gift_orb;
} NPC;

// SRAM SAVE --------------------------------------------------------------
#define SRAM_BASE ((volatile unsigned char*)0x0E000000)
#define SAVE_SIGNATURE "FFGE1"
typedef struct {
    char sig[6];
    int px, py;
    int steps;
    int orbs, wood, stone;
    int comp_count;
    Creature comps[MAX_COMPANIONS];
    int missions[MAX_MISSIONS];
} SaveData;

static void sram_write(const void* src, unsigned int len){
    const unsigned char* s = (const unsigned char*)src;
    volatile unsigned char* d = SRAM_BASE;
    for(unsigned int i=0;i<len;i++) d[i] = s[i];
}
static void sram_read(void* dst, unsigned int len){
    unsigned char* d = (unsigned char*)dst;
    volatile unsigned char* s = SRAM_BASE;
    for(unsigned int i=0;i<len;i++) d[i] = s[i];
}
static int save_game(Player* pl, Creature* comps, int comp_count, Mission* ms){
    SaveData sv; memset(&sv, 0, sizeof(sv));
    strncpy(sv.sig, SAVE_SIGNATURE, 6);
    sv.px=pl->x; sv.py=pl->y; sv.steps=pl->steps;
    sv.orbs=pl->orbs; sv.wood=pl->wood; sv.stone=pl->stone;
    sv.comp_count=comp_count;
    for(int i=0;i<comp_count && i<MAX_COMPANIONS;i++) sv.comps[i]=comps[i];
    for(int i=0;i<MAX_MISSIONS;i++) sv.missions[i]=ms[i].completed;
    sram_write(&sv, sizeof(sv));
    return 1;
}
static int load_game(Player* pl, Creature* comps, int* comp_count, Mission* ms){
    SaveData sv; sram_read(&sv, sizeof(sv));
    if (strncmp(sv.sig, SAVE_SIGNATURE, 5)!=0) return 0;
    pl->x=sv.px; pl->y=sv.py; pl->steps=sv.steps;
    pl->orbs=sv.orbs; pl->wood=sv.wood; pl->stone=sv.stone;
    *comp_count = (sv.comp_count>MAX_COMPANIONS?MAX_COMPANIONS:sv.comp_count);
    for(int i=0;i<*comp_count;i++) comps[i]=sv.comps[i];
    for(int i=0;i<MAX_MISSIONS;i++) ms[i].completed=sv.missions[i];
    return 1;
}

// GLOBALS ----------------------------------------------------------------
static char map_data[MAP_H][MAP_W+1];
static Player player;
static GameState gstate;
static Creature wild;
static Creature boss;
static Creature companions[MAX_COMPANIONS];
static int companion_count=0;
static int sel_build = 0;
static int msg_timer=0;
static int seed0=1;

static NPC npcs[MAX_NPC];
static int npc_count=0;

// Cataloghi --------------------------------------------------------------
static const char* elem_name(ElemType t){
    switch(t){
        case TYPE_FIRE: return "Fuoco";
        case TYPE_WATER: return "Acqua";
        case TYPE_GRASS: return "Erba";
        case TYPE_ELEC: return "Elettro";
        default: return "Neutro";
    }
}

static BuildDef BUILDINGS[] = {
    {"PostoLavoro", 10, 6, TILE_POST},
    {"Torretta",    14, 10, TILE_TOWER},
    {"Farm",        8,  8, TILE_FARM},
    {"Falo",        6,  4, TILE_FIRE},
};
static const int BUILD_COUNT = sizeof(BUILDINGS)/sizeof(BUILDINGS[0]);

static Mission MISSIONS[MAX_MISSIONS] = {
    {"Raccoglitore", "Raccogli 10 Legno e 6 Pietra.", 0},
    {"Banco lavoro", "Costruisci un Posto di lavoro.", 0},
    {"Difesa & Cibo","Costruisci 1 Torretta e 1 Farm.", 0},
    {"Cacciatore",   "Cattura 2 creature diverse.", 0},
    {"Miniboss",     "Sconfiggi il boss notturno nel Bosco.", 0},
};

// RNG & util -------------------------------------------------------------
static inline void wait_vblank(){ VBlankIntrWait(); }
static inline u16 key_down(){ scanKeys(); return keysDown(); }
static inline u16 key_held(){ scanKeys(); return keysHeld(); }

static void seed_rng(){
    int s = REG_VCOUNT ^ (int)rand() ^ ((int)REG_VCOUNT<<16) ^ seed0;
    srand(s);
}
static int rand_range(int a, int b){
    int r = rand() & 0x7fffffff; return a + (r % (b-a+1));
}

static int is_night(){ return (player.steps % 40) >= 30; }

static void cls(){ iprintf("\x1b[2J\x1b[H"); }

// Creature ----------------------------------------------------------------
static Creature make_creature(const char* name, ElemType type, int hp, int atk, int spd, const char* ability){
    Creature c; c.name=name; c.type=type; c.max_hp=hp; c.hp=hp; c.atk=atk; c.speed=spd; c.ability=ability; c.caught=0; return c;
}
static Creature random_wild(){
    int r = rand_range(0,3);
    switch(r){
        case 0: return make_creature("Flarepup", TYPE_FIRE, 24+rand_range(0,6), 6, 6, "Rapido");
        case 1: return make_creature("Aquadine", TYPE_WATER, 28+rand_range(0,6), 5, 5, "Cura");
        case 2: return make_creature("Sproutle", TYPE_GRASS,26+rand_range(0,6), 5, 5, "Tenace");
        default:return make_creature("Voltbit",  TYPE_ELEC, 22+rand_range(0,6), 7, 7, "Rapido");
    }
}
static int type_multiplier(ElemType a, ElemType b){
    if (a==TYPE_FIRE && b==TYPE_GRASS) return 2;
    if (a==TYPE_WATER && b==TYPE_FIRE) return 2;
    if (a==TYPE_GRASS && b==TYPE_WATER) return 2;
    if (a==TYPE_ELEC && b==TYPE_WATER) return 2;
    return 1;
}
static void ability_tick(Creature* c){
    if (strcmp(c->ability,"Cura")==0 && rand_range(0,99)<30){
        c->hp += 2; if (c->hp>c->max_hp) c->hp=c->max_hp;
    }
}

// Map/biomi -----------------------------------------------------------------
static void put_rect(int x0,int y0,int x1,int y1,char ch){
    for(int y=y0;y<=y1;y++) for(int x=x0;x<=x1;x++) map_data[y][x]=ch;
}
static void build_map(){
    // base: prato
    for(int y=0;y<MAP_H;y++){
        for(int x=0;x<MAP_W;x++){
            char t = TILE_GRASS;
            if (y==0||y==MAP_H-1||x==0||x==MAP_W-1) t=TILE_WALL;
            map_data[y][x]=t;
        }
        map_data[y][MAP_W]='\0';
    }
    // bosco (alberi)
    for(int i=0;i<300;i++){
        int x=rand_range(2,MAP_W-3), y=rand_range(2,MAP_H-3);
        if (rand_range(0,100)<60) map_data[y][x]=TILE_TREE;
    }
    // lago
    put_rect(50,6,72,16,TILE_WATER);
    // deserto
    put_rect(8,36,30,56,TILE_SAND);
    // monti / muri
    for(int y=18;y<54;y++) map_data[y][44]=TILE_WALL;
    for(int x=20;x<40;x++) map_data[28][x]=TILE_WALL;

    // base iniziale
    for(int y=2;y<8;y++) for(int x=2;x<10;x++) map_data[y][x] = TILE_BASE;

    // NPC
    npc_count=3;
    npcs[0]=(NPC){6,6,"Saggio", {"Benvenuto, costruttore.","Raccogli legno e pietra.","Apri SELECT per craft."}, 3, 1,3,2,0};
    npcs[1]=(NPC){22,26,"Cacciatrice", {"Di notte emergono nemici.","Una Torretta aiuta molto.","Occhio all'energia."}, 3, 1,0,2,1};
    npcs[2]=(NPC){60,12,"Guardiano", {"Nel bosco a nord-est","si cela un Boss notturno.","Preparati bene."}, 3, 1,0,0,2};

    player.x=4; player.y=4; player.steps=0; player.orbs=1; player.wood=8; player.stone=5;
}

// Rendering ------------------------------------------------------------------
static void draw_view(){
    int vx = player.x - VIEW_W/2; if (vx<0) vx=0; if (vx>MAP_W-VIEW_W) vx=MAP_W-VIEW_W;
    int vy = player.y - VIEW_H/2; if (vy<0) vy=0; if (vy>MAP_H-VIEW_H) vy=MAP_H-VIEW_H;

    iprintf("\x1b[H");
    for(int y=0;y<VIEW_H;y++){
        for(int x=0;x<VIEW_W;x++){
            int mx=vx+x, my=vy+y;
            char ch = map_data[my][mx];
            if (mx==player.x && my==player.y) putchar('P');
            else {
                int nearNpc=0;
                for(int i=0;i<npc_count;i++) if (npcs[i].x==mx && npcs[i].y==my) nearNpc=1;
                if (nearNpc) putchar(TILE_NPC); else putchar(ch);
            }
        }
        putchar('\n');
    }
    iprintf("Legno:%d Pietra:%d Sfere:%d  %s  Sel:%s  \n",
        player.wood, player.stone, player.orbs, is_night()?"Notte":"Giorno", "Edificio");
    iprintf("Compagni:%d  Missioni:START  Craft:SELECT  L+A Costruisci  R+A Cattura  \n", companion_count);
}

static void draw_minimap(){
    int startx = player.x-6; if (startx<0) startx=0; if (startx>MAP_W-12) startx=MAP_W-12;
    int starty = player.y-6; if (starty<0) starty=0; if (starty>MAP_H-12) starty=MAP_H-12;
    for(int y=0;y<12;y++){
        int sy = 1+y;
        iprintf("\x1b[%d;%dH", sy, 19);
        for(int x=0;x<12;x++){
            int mx=startx+x, my=starty+y;
            char ch = map_data[my][mx];
            char m = (mx==player.x && my==player.y) ? '@' :
                      (ch==TILE_GRASS?'g':
                       ch==TILE_TREE?'y':
                       ch==TILE_WATER?'w':
                       ch==TILE_SAND?'s':
                       ch==TILE_WALL?'#':
                       ch==TILE_BASE?'=':
                       ch==TILE_TOWER?'t':
                       ch==TILE_FARM?'f':
                       ch==TILE_FIRE?'h':
                       ch==TILE_POST?'p':'.');
            putchar(m);
        }
    }
}

static void draw_hud(){
    int done=0; for(int i=0;i<MAX_MISSIONS;i++) if (MISSIONS[i].completed) done++;
    iprintf("\x1b[%d;1H", VIEW_H+1);
    iprintf("Missioni: %d/%d   START: Menu (Save/Load/Missioni/Aiuto)              \n", done, MAX_MISSIONS);
}

// Interazioni & logica -------------------------------------------------------
typedef struct { const char* name; int required_wood; int required_stone; char tile_char; } BuildDefLocal;
static BuildDefLocal BUILDINGS_LOCAL[] = {
    {"PostoLavoro", 10, 6, TILE_POST},
    {"Torretta",    14, 10, TILE_TOWER},
    {"Farm",        8,  8, TILE_FARM},
    {"Falo",        6,  4, TILE_FIRE},
};
static int sel_build_idx=0;
static const char* current_build_name(){ return BUILDINGS_LOCAL[sel_build_idx].name; }
static char current_build_tile(){ return BUILDINGS_LOCAL[sel_build_idx].tile_char; }
static int current_build_w(){ return BUILDINGS_LOCAL[sel_build_idx].required_wood; }
static int current_build_s(){ return BUILDINGS_LOCAL[sel_build_idx].required_stone; }

static int can_build_here(char tile){
    if (tile==TILE_EMPTY || tile==TILE_BASE) return 1;
    if (tile==TILE_GRASS || tile==TILE_SAND) return 1;
    return 0;
}
static void try_build(){
    char* cell = &map_data[player.y][player.x];
    if (!can_build_here(*cell)) { iprintf("Non puoi costruire qui.\n"); msg_timer=40; return; }
    if (player.wood < current_build_w() || player.stone < current_build_s()){
        iprintf("Materiali insufficienti per %s.\n", current_build_name()); msg_timer=40; return;
    }
    player.wood -= current_build_w(); player.stone -= current_build_s(); *cell = current_build_tile();
    iprintf("Costruito: %s!\n", current_build_name()); msg_timer=60;
}

static int adjacent(int x1,int y1,int x2,int y2){ int dx=x1-x2; if(dx<0)dx=-dx; int dy=y1-y2; if(dy<0)dy=-dy; return (dx+dy)==1; }

static void gift_from_npc(NPC* n){
    if (n->gave_gift) return;
    n->gave_gift=1;
    player.wood += n->gift_wood;
    player.stone += n->gift_stone;
    player.orbs += n->gift_orb;
}

static void talk_to_nearby_npc(){
    for(int i=0;i<npc_count;i++){
        if (adjacent(player.x,player.y, npcs[i].x,npcs[i].y)){
            cls();
            iprintf("%s:\n\n", npcs[i].name);
            for(int l=0;l<npcs[i].line_count;l++) iprintf("  %s\n", npcs[i].lines[l]);
            gift_from_npc(&npcs[i]);
            iprintf("\nHai ricevuto: +%d Legno, +%d Pietra, +%d Sfera.\n", npcs[i].gift_wood, npcs[i].gift_stone, npcs[i].gift_orb);
            iprintf("\nPremi A per continuare.");
            while(1){ if (key_down() & KEY_A) break; wait_vblank(); }
            return;
        }
    }
    iprintf("Non c'e' nessuno con cui parlare qui.\n"); msg_timer=30;
}

static void try_gather_or_action(){
    char* cell = &map_data[player.y][player.x];
    if (*cell==TILE_WALL){ iprintf("Una parete blocca il passaggio.\n"); msg_timer=40; return; }
    if (*cell==TILE_WATER){ iprintf("L'acqua ti ostruisce.\n"); msg_timer=40; return; }
    if (*cell==TILE_TREE){
        if (rand_range(0,99)<70){ player.wood++; iprintf("Tagli un ramo: +1 Legno.\n"); }
        else iprintf("L'albero resiste.\n");
        msg_timer=30; return;
    }
    if (*cell==TILE_GRASS || *cell==TILE_SAND){
        if (rand_range(0,99)<18){ wild=random_wild(); gstate=GS_BATTLE; return; }
        iprintf("Fruscio... nessun incontro.\n"); msg_timer=20; return;
    }
    if (*cell==TILE_POST){
        int bonus = companion_count>0 ? 1 : 0;
        int roll = rand_range(0,1);
        if (roll==0){ player.wood += 1+bonus; iprintf("+%d Legno dal Posto di lavoro.\n", 1+bonus); }
        else { player.stone += 1+bonus; iprintf("+%d Pietra dal Posto di lavoro.\n", 1+bonus); }
        msg_timer=30; return;
    }
    if (*cell==TILE_FARM){ player.wood += 1; iprintf("+1 Legno dalla Farm.\n"); msg_timer=20; return; }
    if (*cell==TILE_FIRE){
        for(int i=0;i<companion_count;i++){ companions[i].hp += 4; if (companions[i].hp>companions[i].max_hp) companions[i].hp=companions[i].max_hp; }
        iprintf("Falò caldo: i compagni si curano.\n"); msg_timer=30; return;
    }
    talk_to_nearby_npc();
}

static void try_craft_quick(){
    if (player.wood>=5 && player.stone>=3){ player.wood-=5; player.stone-=3; player.orbs++; iprintf("Craft: Sfera +1 (tot %d)\n", player.orbs); msg_timer=30; return; }
    if (player.wood>=10 && player.stone>=6){
        char* cell = &map_data[player.y][player.x];
        if (can_build_here(*cell)){ player.wood-=10; player.stone-=6; *cell=TILE_POST; iprintf("Posto di lavoro posizionato.\n"); msg_timer=30; return; }
    }
    iprintf("Materiali insufficienti per craft rapido.\n"); msg_timer=30;
}

static void try_throw_orb(){
    if (player.orbs<=0){ iprintf("Non hai Sfere. Craft con SELECT.\n"); msg_timer=40; return; }
    if (rand_range(0,99)<12){ wild=random_wild(); gstate=GS_BATTLE; iprintf("Una creatura appare!"); }
    else { iprintf("Lanci una Sfera a vuoto."); player.orbs--; }
    msg_timer=30;
}

// Battaglie ------------------------------------------------------------------
static int type_mul(ElemType a, ElemType b){ return type_multiplier(a,b); }

static void battle_intro(){
    cls();
    iprintf("Un %s (%s) selvatico appare!\n\n", wild.name, elem_name(wild.type));
    iprintf("HP: %d/%d  Abilita: %s\n\n", wild.hp, wild.max_hp, wild.ability);
    iprintf("  > Attacco rapido\n");
    iprintf("    Mossa speciale\n");
    iprintf("    Cattura\n");
    iprintf("    Fuggi\n");
}
static int battle_menu(int sel){
    int base=5;
    const char* labels[4] = {"Attacco rapido","Mossa speciale","Cattura","Fuggi"};
    for(int i=0;i<4;i++){ iprintf("\x1b[%d;1H", base+i); iprintf("%c %s  ", (i==sel?'>':' '), labels[i]); }
    return sel;
}
static int do_battle(){
    int sel=0; battle_intro(); battle_menu(sel);
    while(1){
        u16 kd = key_down();
        if (kd & KEY_UP)   { if(--sel<0) sel=3; battle_menu(sel); }
        if (kd & KEY_DOWN) { if(++sel>3) sel=0; battle_menu(sel); }
        if (kd & KEY_A){
            if (sel==0){ int mult=type_mul(TYPE_NEUTRAL,wild.type); int dmg=4*mult + (is_night()?1:0); wild.hp-=dmg; if (wild.hp<0) wild.hp=0; iprintf("\x1b[10;1HColpisci per %d.       ", dmg); }
            if (sel==1){ ElemType et=(companion_count>0?companions[0].type:TYPE_GRASS); int mult=type_mul(et,wild.type); int dmg=6*mult; wild.hp-=dmg; if (wild.hp<0) wild.hp=0; iprintf("\x1b[11;1HMossa %s: %d.      ", elem_name(et), dmg); }
            if (sel==2){
                if (player.orbs<=0){ iprintf("\x1b[12;1HNiente Sfere! "); }
                else { int chance=(wild.max_hp-wild.hp)*100/(wild.max_hp+1)+10; int roll=rand_range(0,99); player.orbs--; iprintf("\x1b[12;1HLancio... (%d vs %d) ", roll, chance); if (roll<=chance){ iprintf("\nCatturato %s! Premi A...", wild.name); while(1){ if (key_down() & KEY_A) break; wait_vblank(); } if (companion_count<MAX_COMPANIONS){ companions[companion_count++]=wild; companions[companion_count-1].caught=1; } return 3; } else { iprintf("\nSi libera! "); } }
            }
            if (sel==3) return 0;
            if (wild.hp==0){ iprintf("\nSconfitto! Premi A..."); while(1){ if (key_down() & KEY_A) break; wait_vblank(); } return 2; }
        }
        if (kd & KEY_B) return 0;
        wait_vblank();
    }
}

// BOSS (semplice trigger)
static int near_boss_area(){ return (player.x>55 && player.x<75 && player.y>6 && player.y<20); }

// Menu START -----------------------------------------------------------------
static void show_menu(){
    while(1){
        cls();
        iprintf("FaunaFrontierGBA — Menu\n");
        iprintf("------------------------\n\n");
        iprintf("A) Missioni & Aiuto\n");
        iprintf("SELECT) SAVE   R) LOAD\n");
        iprintf("B/START) Indietro\n");
        while(1){
            u16 kd = key_down();
            if (kd & (KEY_B|KEY_START)) return;
            if (kd & KEY_A){
                cls();
                iprintf("Missioni:\n\n");
                for(int i=0;i<MAX_MISSIONS;i++){ iprintf("[%c] %s - %s\n", 0?'X':' ', MISSIONS[i].title, MISSIONS[i].desc); }
                iprintf("\n- Muoviti, raccogli Legno/Pietra.\n- SELECT: craft rapido (Sfere, Posto).\n- L/R: edificio selezionato; L+A costruisci.\n- R+A lancia Sfera.\n- NPC danno indizi e doni.\n\nB/START per uscire.");
                while(1){ u16 k2=key_down(); if (k2 & (KEY_B|KEY_START)) break; wait_vblank(); }
                break;
            }
            if (kd & KEY_SELECT){ save_game(&player, companions, companion_count, MISSIONS); iprintf("\nSalvato su SRAM!"); }
            if (kd & KEY_R){ int ok=load_game(&player, companions, &companion_count, MISSIONS); iprintf(ok? "\nCaricato da SRAM!":"\nNessun salvataggio."); }
            wait_vblank();
        }
    }
}

// Game loop ------------------------------------------------------------------
int main(void){
    irqInit(); irqEnable(IRQ_VBLANK); consoleDemoInit(); seed_rng(); build_map(); gstate=GS_WORLD;

    cls();
    iprintf("FaunaFrontierGBA — Enhanced\nPremi A per iniziare...");
    while(1){ if (key_down() & KEY_A) break; wait_vblank(); }
    cls();

    int move_cd=0;

    while(1){
        u16 kd = key_down();
        u16 held = key_held();

        if (kd & KEY_START){ show_menu(); cls(); }
        if (kd & KEY_SELECT){ try_craft_quick(); }
        if (kd & KEY_L){ sel_build_idx = (sel_build_idx-1+4)%4; }
        if (kd & KEY_R){ sel_build_idx = (sel_build_idx+1)%4; }
        if ((held & KEY_R) && (kd & KEY_A)){ try_throw_orb(); }
        if ((held & KEY_L) && (kd & KEY_A)){ try_build(); }

        if (gstate==GS_WORLD){
            if (move_cd>0) move_cd--;
            int dx=0, dy=0;
            if (move_cd==0){
                if (held & KEY_UP){ dy=-1; move_cd=3; }
                else if (held & KEY_DOWN){ dy=1; move_cd=3; }
                else if (held & KEY_LEFT){ dx=-1; move_cd=3; }
                else if (held & KEY_RIGHT){ dx=1; move_cd=3; }
                if (dx||dy){
                    int nx=player.x+dx, ny=player.y+dy;
                    if (nx>=1 && nx<MAP_W-1 && ny>=1 && ny<MAP_H-1){
                        if (map_data[ny][nx]!=TILE_WALL && map_data[ny][nx]!=TILE_WATER){ player.x=nx; player.y=ny; player.steps++; }
                    }
                }
            }
            if (kd & KEY_A){ try_gather_or_action(); }

            if (is_night() && near_boss_area()){
                // Simple boss trigger: bonus loot
                if (rand_range(0,99)<5){ player.orbs += 2; iprintf("Hai trovato tracce del Boss. +2 Sfere!"); msg_timer=40; }
            }

            draw_view();
            draw_minimap();
            draw_hud();
            if (msg_timer>0) msg_timer--;
        } else if (gstate==GS_BATTLE){
            int res = do_battle(); (void)res;
            gstate = GS_WORLD;
            cls();
        }

        wait_vblank();
    }
    return 0;
}
