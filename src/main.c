#include "raylib.h"
#include "raymath.h"
#include <math.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>

//------------------------------------------------------------------------------------
// Config / tuning
//------------------------------------------------------------------------------------
#define SCREEN_W 1280
#define SCREEN_H 720

#define GRAVITY           1500.0f
#define MAX_FALL_SPEED    1100.0f

#define GROUND_ACCEL      2600.0f
#define AIR_ACCEL         1500.0f
#define GROUND_FRICTION   3200.0f
#define AIR_FRICTION      500.0f
#define MAX_RUN_SPEED     420.0f

#define JUMP_VELOCITY     -640.0f
#define DOUBLE_JUMP_VELOCITY -560.0f
#define JUMP_CUT_MULT     0.45f
#define COYOTE_TIME       0.10f
#define JUMP_BUFFER_TIME  0.12f

#define DASH_SPEED        950.0f  // fixed horizontal speed during a dash burst
#define DASH_DURATION     0.16f   // how long the burst lasts
#define DASH_GRAVITY_MULT 0.15f   // gravity is mostly suspended during the burst

#define PLAYER_W 30.0f
#define PLAYER_H 44.0f

#define GRAPPLE_RANGE     620.0f
#define GRAPPLE_MIN_LEN   70.0f
#define GRAPPLE_REEL_SPD  260.0f
#define GRAPPLE_PULL_ACC  1800.0f

#define WALL_BOUNCE_SPEED_THRESHOLD  480.0f
#define WALL_BOUNCE_RESTITUTION      0.55f

#define WORLD_DEATH_Y     1400.0f

#define MAX_SOLIDS  140
#define MAX_SPIKES  60
#define MAX_ANCHORS 48
#define MAX_COINS   100
#define MAX_ENEMIES 60
#define MAX_PARTICLES 300
#define LEVEL_COUNT 5
#define CUSTOM_LEVEL_INDEX (-1)

#define MPH_SCALE            12.0f
#define CAMERA_LEAD_FACTOR   0.35f
#define CAMERA_LAG_SPEED     6.0f
#define SPEED_BLUR_MIN       300.0f
#define SPEED_BLUR_MAX       900.0f
#define MOTION_TRAIL_COUNT   6
#define MAX_STREAKS          80

typedef enum { STATE_MENU, STATE_LEVEL_SELECT, STATE_PLAYING, STATE_EDITOR } GameState;

typedef enum {
    TOOL_GROUND, TOOL_FLOATING, TOOL_WALL, TOOL_SPIKE, TOOL_ANCHOR,
    TOOL_COIN, TOOL_ENEMY, TOOL_GOAL, TOOL_SPAWN
} EditorTool;
#define NUM_EDITOR_TOOLS 9

typedef enum {
    SOLID_GROUND, SOLID_FLOATING, SOLID_WALL,
    SOLID_MOVING, SOLID_BREAKABLE, SOLID_PHASING
} SolidType;

typedef struct {
    Rectangle rect;
    SolidType type;

    Vector2 moveOrigin;
    Vector2 moveVec;
    float   moveSpeed;
    float   movePhase;
    float   moveDir;
    Vector2 velocity;

    float breakTimer;
    float breakDelay;
    bool  broken;
    float respawnTimer;

    float phaseTimer;
    float phaseOnTime;
    float phaseOffTime;
    float phaseOffset;
    bool  phasedOut;
} Solid;

typedef struct {
    Vector2 position;
    Vector2 velocity;
    bool onGround;
    bool grappling;
    Vector2 grappleAnchor;
    float ropeLength;
    float facing;

    float coyoteTimer;
    float jumpBufferTimer;
    bool  usedDoubleJump;
    bool  usedDash;
    float dashTimer;     // > 0 while a dash burst is active
    float dashDir;       // locked burst direction, set when the dash fires
    float dashSpeed;     // locked burst magnitude: max(DASH_SPEED, speed you already had)
    Vector2 scale;

    Vector2 ropeDir;
    bool    ropeDirValid;
} Player;

typedef struct {
    Vector2 pos;
    Vector2 vel;
    float life, maxLife;
    float size;
    Color color;
    bool alive;
} Particle;

typedef struct {
    Vector2 pos;
    bool collected;
    float bob;
} Coin;

#define ENEMY_W 30.0f
#define ENEMY_H 26.0f
typedef struct {
    Vector2 pos;
    float minX, maxX;
    float speed;
    float dir;
} Enemy;

typedef struct {
    Vector2 pos;
    float life, maxLife;
    float speed;
    Color color;
    bool alive;
} SpeedStreak;

static SpeedStreak streaks[MAX_STREAKS];

typedef struct {
    Vector2 pos;
    float life;
    Vector2 scale;
    float facing;
    bool grappling;
} TrailGhost;

static TrailGhost trailGhosts[MOTION_TRAIL_COUNT];
static float trailTimer = 0.0f;

static Vector2 cameraSmooth = { 0 };
static Vector2 cameraVel = { 0 };
static float speedIntensity = 0.0f;
static float displaySpeed = 0.0f;

static Solid   solids[MAX_SOLIDS];
static int     solidCount = 0;
static Rectangle spikes[MAX_SPIKES];
static int     spikeCount = 0;
static Vector2 anchors[MAX_ANCHORS];
static int     anchorCount = 0;
static Coin    coins[MAX_COINS];
static int     coinCount = 0;
static int     coinsCollected = 0;
static Enemy   enemies[MAX_ENEMIES];
static int     enemyCount = 0;
static Rectangle goalRect;
static Vector2 spawnPoint;
static float   worldWidth = 6000.0f;
static float   worldTopY = -60.0f;
static float   worldBottomY = 1000.0f;

static Particle particles[MAX_PARTICLES];

static float shakeTime = 0.0f;
static float shakeMagnitude = 0.0f;

static float levelTime = 0.0f;
static float bestTimes[LEVEL_COUNT] = { -1.0f, -1.0f, -1.0f, -1.0f, -1.0f };
static int currentLevel = 0;
static const char* levelNames[LEVEL_COUNT] = {
    "Momentum Run", "Spike Alley", "Sky Islands", "Twin Lanes", "Clockwork Spire"
};

//------------------------------------------------------------------------------------
// Background theme + per-level solid palette
//------------------------------------------------------------------------------------
typedef enum {
    BG_DEFAULT,   // level 1 - green meadows
    BG_CAVERN,    // level 2 - red rock / lava cavern
    BG_SKY,       // level 3 - floating islands, blue sky + clouds
    BG_CITY,      // level 4 - dusk city skyline
    BG_TOWER,     // level 5 - brass clocktower
    BG_EDITOR     // editor - neutral grid
} BackgroundTheme;

static BackgroundTheme currentBgTheme = BG_DEFAULT;

typedef struct {
    Color ground;
    Color floating;
    Color wall;
    Color moving;
    Color breakable;
    Color phasing;
    Color groundTop;   // the top "cap" stripe on ground tiles
} SolidPalette;

static const SolidPalette paletteMeadow = {
    {  80, 130,  80, 255 }, { 120, 160, 220, 255 }, {  90,  90, 100, 255 },
    {  80, 180, 200, 255 }, { 190, 140,  80, 255 }, { 170, 110, 220, 255 },
    { 110, 190,  90, 255 }
};
static const SolidPalette paletteCavern = {
    { 118,  58,  44, 255 }, { 158,  86,  60, 255 }, {  70,  40,  34, 255 },
    { 190, 110,  60, 255 }, { 210, 140,  70, 255 }, { 200,  80,  70, 255 },
    { 190, 100,  60, 255 }
};
static const SolidPalette paletteSky = {
    { 200, 214, 232, 255 }, { 178, 200, 228, 255 }, { 128, 140, 170, 255 },
    { 140, 210, 240, 255 }, { 230, 210, 180, 255 }, { 200, 180, 240, 255 },
    { 240, 246, 255, 255 }
};
static const SolidPalette paletteCity = {
    {  74,  86, 108, 255 }, { 108, 130, 160, 255 }, {  46,  54,  72, 255 },
    { 100, 180, 220, 255 }, { 200, 150,  90, 255 }, { 200, 120, 190, 255 },
    { 140, 200, 240, 255 }
};
static const SolidPalette paletteTower = {
    { 120,  78,  42, 255 }, { 150, 108,  60, 255 }, {  70,  44,  26, 255 },
    { 190, 150,  80, 255 }, { 210, 160,  80, 255 }, { 200, 170, 110, 255 },
    { 220, 180, 110, 255 }
};
static const SolidPalette paletteEditor = {
    {  80, 130,  80, 255 }, { 120, 160, 220, 255 }, {  90,  90, 100, 255 },
    {  80, 180, 200, 255 }, { 190, 140,  80, 255 }, { 170, 110, 220, 255 },
    { 110, 190,  90, 255 }
};

// Inline initialiser avoids the C restriction that a mutable global cannot be
// initialised with another aggregate. Same values as paletteMeadow.
static SolidPalette currentPalette = {
    {  80, 130,  80, 255 }, { 120, 160, 220, 255 }, {  90,  90, 100, 255 },
    {  80, 180, 200, 255 }, { 190, 140,  80, 255 }, { 170, 110, 220, 255 },
    { 110, 190,  90, 255 }
};

//------------------------------------------------------------------------------------
// Level editor state
//------------------------------------------------------------------------------------
#define EDITOR_GRID        20.0f
#define EDITOR_PICK_RADIUS 26.0f
#define CUSTOM_LEVEL_FILE  "custom_level.txt"

static EditorTool editorTool = TOOL_GROUND;
static Vector2 editorCamTarget = { 400.0f, 500.0f };
static float   editorZoom = 1.0f;
static bool    editorDragging = false;
static Vector2 editorDragStart = { 0 };
static char    editorStatusMsg[80] = "";
static float   editorStatusTimer = 0.0f;

static const char* editorToolLabels[NUM_EDITOR_TOOLS] = {
    "GROUND", "FLOAT", "WALL", "SPIKE", "ANCHOR", "COIN", "ENEMY", "GOAL", "SPAWN"
};

static Sound sndJump, sndDoubleJump, sndLand, sndGrapple, sndWallBounce, sndCoin, sndWin, sndDeath, sndDash;
static bool audioReady = false;

static void AddSolid(float x, float y, float w, float h, SolidType type)
{
    if (solidCount >= MAX_SOLIDS) return;
    Solid* s = &solids[solidCount];
    memset(s, 0, sizeof(Solid));
    s->rect = (Rectangle){ x, y, w, h };
    s->type = type;
    s->moveDir = 1.0f;
    solidCount++;
}

static void AddMovingPlatform(float x, float y, float w, float h,
    float dx, float dy, float speed, float phase)
{
    if (solidCount >= MAX_SOLIDS) return;
    Solid* s = &solids[solidCount];
    memset(s, 0, sizeof(Solid));
    s->rect = (Rectangle){ x, y, w, h };
    s->type = SOLID_MOVING;
    s->moveOrigin = (Vector2){ x, y };
    s->moveVec = (Vector2){ dx, dy };
    s->moveSpeed = speed;
    s->movePhase = phase;
    s->moveDir = 1.0f;
    solidCount++;
}

static void AddBreakablePlatform(float x, float y, float w, float h, float delay)
{
    if (solidCount >= MAX_SOLIDS) return;
    Solid* s = &solids[solidCount];
    memset(s, 0, sizeof(Solid));
    s->rect = (Rectangle){ x, y, w, h };
    s->type = SOLID_BREAKABLE;
    s->breakDelay = delay;
    s->moveDir = 1.0f;
    solidCount++;
}

static void AddPhasingPlatform(float x, float y, float w, float h,
    float onTime, float offTime, float phase)
{
    if (solidCount >= MAX_SOLIDS) return;
    Solid* s = &solids[solidCount];
    memset(s, 0, sizeof(Solid));
    s->rect = (Rectangle){ x, y, w, h };
    s->type = SOLID_PHASING;
    s->phaseOnTime = onTime;
    s->phaseOffTime = offTime;
    s->phaseOffset = phase;
    s->moveDir = 1.0f;
    solidCount++;
}

static void AddSpike(float x, float y, float w, float h)
{
    if (spikeCount >= MAX_SPIKES) return;
    spikes[spikeCount++] = (Rectangle){ x, y, w, h };
}

static void AddAnchor(float x, float y)
{
    if (anchorCount >= MAX_ANCHORS) return;
    anchors[anchorCount++] = (Vector2){ x, y };
}

static void AddCoin(float x, float y)
{
    if (coinCount >= MAX_COINS) return;
    coins[coinCount].pos = (Vector2){ x, y };
    coins[coinCount].collected = false;
    coins[coinCount].bob = (float)(coinCount) * 0.6f;
    coinCount++;
}

static void AddEnemy(float x, float groundY, float minX, float maxX, float speed)
{
    if (enemyCount >= MAX_ENEMIES) return;
    enemies[enemyCount].pos = (Vector2){ x, groundY };
    enemies[enemyCount].minX = minX;
    enemies[enemyCount].maxX = maxX;
    enemies[enemyCount].speed = speed;
    enemies[enemyCount].dir = 1.0f;
    enemyCount++;
}

static void EditorStatus(const char* msg)
{
    snprintf(editorStatusMsg, sizeof(editorStatusMsg), "%s", msg);
    editorStatusTimer = 2.2f;
}

static float EditorSnap(float v) { return roundf(v / EDITOR_GRID) * EDITOR_GRID; }

static void RecalcCustomWorldBounds(void)
{
    float minX = spawnPoint.x, maxX = spawnPoint.x + 200.0f;
    float minY = spawnPoint.y - 300.0f, maxY = spawnPoint.y + 300.0f;

#define EXPAND(px, py) do { \
        if ((px) < minX) minX = (px); if ((px) > maxX) maxX = (px); \
        if ((py) < minY) minY = (py); if ((py) > maxY) maxY = (py); \
    } while (0)

    EXPAND(goalRect.x, goalRect.y);
    EXPAND(goalRect.x + goalRect.width, goalRect.y + goalRect.height);
    for (int i = 0; i < solidCount; i++)
    {
        EXPAND(solids[i].rect.x, solids[i].rect.y);
        EXPAND(solids[i].rect.x + solids[i].rect.width, solids[i].rect.y + solids[i].rect.height);
    }
    for (int i = 0; i < spikeCount; i++)
    {
        EXPAND(spikes[i].x, spikes[i].y);
        EXPAND(spikes[i].x + spikes[i].width, spikes[i].y + spikes[i].height);
    }
    for (int i = 0; i < anchorCount; i++) EXPAND(anchors[i].x, anchors[i].y);
    for (int i = 0; i < coinCount; i++) EXPAND(coins[i].pos.x, coins[i].pos.y);
    for (int i = 0; i < enemyCount; i++) EXPAND(enemies[i].pos.x, enemies[i].pos.y);
#undef EXPAND

    worldWidth = maxX + 400.0f;
    worldTopY = minY - 300.0f;
    worldBottomY = maxY + 300.0f;
}

static void ResetCustomLevelScaffold(void)
{
    solidCount = spikeCount = anchorCount = coinCount = enemyCount = 0;
    AddSolid(0, 650, 600, 100, SOLID_GROUND);
    spawnPoint = (Vector2){ 60, 650 - PLAYER_H };
    goalRect = (Rectangle){ 2000, 580, 40, 70 };
    RecalcCustomWorldBounds();
}

static bool TryEraseNear(Vector2 world)
{
    for (int i = solidCount - 1; i >= 0; i--)
        if (CheckCollisionPointRec(world, solids[i].rect)) { solids[i] = solids[--solidCount]; return true; }
    for (int i = spikeCount - 1; i >= 0; i--)
        if (CheckCollisionPointRec(world, spikes[i])) { spikes[i] = spikes[--spikeCount]; return true; }
    for (int i = anchorCount - 1; i >= 0; i--)
        if (Vector2Distance(world, anchors[i]) < EDITOR_PICK_RADIUS) { anchors[i] = anchors[--anchorCount]; return true; }
    for (int i = coinCount - 1; i >= 0; i--)
        if (Vector2Distance(world, coins[i].pos) < EDITOR_PICK_RADIUS) { coins[i] = coins[--coinCount]; return true; }
    for (int i = enemyCount - 1; i >= 0; i--)
        if (Vector2Distance(world, enemies[i].pos) < EDITOR_PICK_RADIUS * 1.3f) { enemies[i] = enemies[--enemyCount]; return true; }
    return false;
}

static void SaveCustomLevel(void)
{
    FILE* f = fopen(CUSTOM_LEVEL_FILE, "w");
    if (!f) { EditorStatus("Save failed (couldn't open file)"); return; }

    fprintf(f, "CUSTOMLEVEL 1\n");
    fprintf(f, "SPAWN %f %f\n", spawnPoint.x, spawnPoint.y);
    fprintf(f, "GOAL %f %f %f %f\n", goalRect.x, goalRect.y, goalRect.width, goalRect.height);
    fprintf(f, "SOLIDS %d\n", solidCount);
    for (int i = 0; i < solidCount; i++)
        fprintf(f, "%f %f %f %f %d\n", solids[i].rect.x, solids[i].rect.y,
            solids[i].rect.width, solids[i].rect.height, (int)solids[i].type);
    fprintf(f, "SPIKES %d\n", spikeCount);
    for (int i = 0; i < spikeCount; i++)
        fprintf(f, "%f %f %f %f\n", spikes[i].x, spikes[i].y, spikes[i].width, spikes[i].height);
    fprintf(f, "ANCHORS %d\n", anchorCount);
    for (int i = 0; i < anchorCount; i++)
        fprintf(f, "%f %f\n", anchors[i].x, anchors[i].y);
    fprintf(f, "COINS %d\n", coinCount);
    for (int i = 0; i < coinCount; i++)
        fprintf(f, "%f %f\n", coins[i].pos.x, coins[i].pos.y);
    fprintf(f, "ENEMIES %d\n", enemyCount);
    for (int i = 0; i < enemyCount; i++)
        fprintf(f, "%f %f %f %f %f\n", enemies[i].pos.x, enemies[i].pos.y,
            enemies[i].minX, enemies[i].maxX, enemies[i].speed);

    fclose(f);
    EditorStatus("Saved!");
}

static bool LoadCustomLevel(void)
{
    FILE* f = fopen(CUSTOM_LEVEL_FILE, "r");
    if (!f) return false;

    char tag[32];
    int version = 0;
    if (fscanf(f, "%31s %d", tag, &version) != 2 || strcmp(tag, "CUSTOMLEVEL") != 0)
    {
        fclose(f);
        return false;
    }

    solidCount = spikeCount = anchorCount = coinCount = enemyCount = 0;

    float sx, sy, gx, gy, gw, gh;
    fscanf(f, "%31s %f %f", tag, &sx, &sy);
    spawnPoint = (Vector2){ sx, sy };
    fscanf(f, "%31s %f %f %f %f", tag, &gx, &gy, &gw, &gh);
    goalRect = (Rectangle){ gx, gy, gw, gh };

    int n = 0;
    fscanf(f, "%31s %d", tag, &n);
    for (int i = 0; i < n; i++)
    {
        float x, y, w, h; int type;
        if (fscanf(f, "%f %f %f %f %d", &x, &y, &w, &h, &type) != 5) break;
        AddSolid(x, y, w, h, (SolidType)type);
    }
    fscanf(f, "%31s %d", tag, &n);
    for (int i = 0; i < n; i++)
    {
        float x, y, w, h;
        if (fscanf(f, "%f %f %f %f", &x, &y, &w, &h) != 4) break;
        AddSpike(x, y, w, h);
    }
    fscanf(f, "%31s %d", tag, &n);
    for (int i = 0; i < n; i++)
    {
        float x, y;
        if (fscanf(f, "%f %f", &x, &y) != 2) break;
        AddAnchor(x, y);
    }
    fscanf(f, "%31s %d", tag, &n);
    for (int i = 0; i < n; i++)
    {
        float x, y;
        if (fscanf(f, "%f %f", &x, &y) != 2) break;
        AddCoin(x, y);
    }
    fscanf(f, "%31s %d", tag, &n);
    for (int i = 0; i < n; i++)
    {
        float x, y, minX, maxX, speed;
        if (fscanf(f, "%f %f %f %f %f", &x, &y, &minX, &maxX, &speed) != 5) break;
        AddEnemy(x, y, minX, maxX, speed);
    }

    fclose(f);
    RecalcCustomWorldBounds();
    return true;
}

static void UpdateEnemies(float dt)
{
    for (int i = 0; i < enemyCount; i++)
    {
        enemies[i].pos.x += enemies[i].dir * enemies[i].speed * dt;
        if (enemies[i].pos.x < enemies[i].minX) { enemies[i].pos.x = enemies[i].minX; enemies[i].dir = 1.0f; }
        if (enemies[i].pos.x > enemies[i].maxX) { enemies[i].pos.x = enemies[i].maxX; enemies[i].dir = -1.0f; }
    }
}

static Rectangle EnemyRect(Enemy* e)
{
    return (Rectangle) { e->pos.x - ENEMY_W * 0.5f, e->pos.y - ENEMY_H, ENEMY_W, ENEMY_H };
}

static void DrawEnemies(float t)
{
    for (int i = 0; i < enemyCount; i++)
    {
        Rectangle r = EnemyRect(&enemies[i]);
        float bob = sinf(t * 8.0f + i * 1.7f) * 2.0f;
        Rectangle body = { r.x, r.y + bob, r.width, r.height };

        // Dark silhouette ring so the enemy reads against any background
        DrawEllipse((int)(body.x + body.width * 0.5f), (int)(body.y + body.height * 0.6f),
            body.width * 0.6f, body.height * 0.6f, (Color) { 20, 4, 26, 255 });
        DrawEllipse((int)(body.x + body.width * 0.5f), (int)(body.y + body.height * 0.6f),
            body.width * 0.5f, body.height * 0.5f, (Color) { 130, 40, 150, 255 });
        DrawEllipseLines((int)(body.x + body.width * 0.5f), (int)(body.y + body.height * 0.6f),
            body.width * 0.5f, body.height * 0.5f, (Color) { 30, 5, 35, 255 });

        for (int k = 0; k < 3; k++)
        {
            float sx = body.x + body.width * (0.2f + 0.3f * k);
            DrawTriangle((Vector2) { sx - 4, body.y + body.height * 0.35f },
                (Vector2) {
                sx + 4, body.y + body.height * 0.35f
            },
                (Vector2) {
                sx, body.y - 4
            },
                (Color) {
                90, 20, 110, 255
            });
        }

        float eyeDir = enemies[i].dir;
        DrawCircle((int)(body.x + body.width * 0.5f + eyeDir * 5.0f), (int)(body.y + body.height * 0.55f), 3.5f, BLACK);
        DrawCircle((int)(body.x + body.width * 0.5f + eyeDir * 5.0f), (int)(body.y + body.height * 0.55f), 3.0f, WHITE);
        DrawCircle((int)(body.x + body.width * 0.5f + eyeDir * 5.0f), (int)(body.y + body.height * 0.55f), 1.3f, BLACK);
    }
}

static void SpawnParticle(Vector2 pos, Vector2 vel, float life, float size, Color color)
{
    for (int i = 0; i < MAX_PARTICLES; i++)
    {
        if (!particles[i].alive)
        {
            particles[i].alive = true;
            particles[i].pos = pos;
            particles[i].vel = vel;
            particles[i].life = particles[i].maxLife = life;
            particles[i].size = size;
            particles[i].color = color;
            return;
        }
    }
}

static void SpawnBurst(Vector2 pos, int count, float speed, float life, float size, Color color)
{
    for (int i = 0; i < count; i++)
    {
        float a = ((float)GetRandomValue(0, 360)) * DEG2RAD;
        float s = speed * (0.4f + 0.6f * (GetRandomValue(0, 100) / 100.0f));
        Vector2 v = { cosf(a) * s, sinf(a) * s - speed * 0.3f };
        SpawnParticle(pos, v, life * (0.6f + 0.4f * (GetRandomValue(0, 100) / 100.0f)), size, color);
    }
}

static void UpdateParticles(float dt)
{
    for (int i = 0; i < MAX_PARTICLES; i++)
    {
        if (!particles[i].alive) continue;
        particles[i].life -= dt;
        if (particles[i].life <= 0.0f) { particles[i].alive = false; continue; }
        particles[i].vel.y += 900.0f * dt;
        particles[i].pos = Vector2Add(particles[i].pos, Vector2Scale(particles[i].vel, dt));
    }
}

static void DrawParticles(void)
{
    for (int i = 0; i < MAX_PARTICLES; i++)
    {
        if (!particles[i].alive) continue;
        float t = particles[i].life / particles[i].maxLife;
        Color c = particles[i].color;
        c.a = (unsigned char)(255 * t);
        DrawCircleV(particles[i].pos, particles[i].size * t, c);
    }
}

static void Shake(float mag, float time)
{
    if (mag > shakeMagnitude) shakeMagnitude = mag;
    if (time > shakeTime) shakeTime = time;
}

static Sound MakeTone(float freq, float duration, float freqSlide, bool square)
{
    int sampleRate = 44100;
    int frameCount = (int)(duration * sampleRate);
    if (frameCount < 1) frameCount = 1;
    short* data = (short*)malloc(sizeof(short) * frameCount);

    for (int i = 0; i < frameCount; i++)
    {
        float t = (float)i / sampleRate;
        float f = freq + freqSlide * t;
        float phase = 2.0f * PI * f * t;
        float value = square ? (sinf(phase) >= 0.0f ? 1.0f : -1.0f) : sinf(phase);
        float env = 1.0f - (float)i / (float)frameCount;
        env = env * env;
        data[i] = (short)(value * env * 4000.0f);
    }

    Wave wave = { 0 };
    wave.frameCount = frameCount;
    wave.sampleRate = sampleRate;
    wave.sampleSize = 16;
    wave.channels = 1;
    wave.data = data;

    Sound s = LoadSoundFromWave(wave);
    UnloadWave(wave);
    return s;
}

static void InitGameAudio(void)
{
    InitAudioDevice();
    audioReady = IsAudioDeviceReady();
    if (!audioReady) return;

    sndJump = MakeTone(420.0f, 0.12f, 260.0f, true);
    sndDoubleJump = MakeTone(560.0f, 0.14f, 420.0f, true);
    sndLand = MakeTone(140.0f, 0.08f, -60.0f, true);
    sndGrapple = MakeTone(700.0f, 0.10f, 180.0f, true);
    sndWallBounce = MakeTone(260.0f, 0.10f, 120.0f, true);
    sndCoin = MakeTone(900.0f, 0.14f, 700.0f, false);
    sndWin = MakeTone(600.0f, 0.55f, 500.0f, false);
    sndDeath = MakeTone(300.0f, 0.30f, -220.0f, true);
    sndDash = MakeTone(220.0f, 0.10f, 900.0f, false);
}

static void ShutdownGameAudio(void)
{
    if (!audioReady) return;
    UnloadSound(sndJump);
    UnloadSound(sndDoubleJump);
    UnloadSound(sndLand);
    UnloadSound(sndGrapple);
    UnloadSound(sndWallBounce);
    UnloadSound(sndCoin);
    UnloadSound(sndWin);
    UnloadSound(sndDeath);
    UnloadSound(sndDash);
    CloseAudioDevice();
}

static void Snd(Sound s) { if (audioReady) PlaySound(s); }

static void BuildLevel1(void)
{
    AddSolid(0, 650, 520, 100, SOLID_GROUND);
    AddCoin(260, 580);
    AddSolid(660, 650, 380, 100, SOLID_GROUND);
    AddCoin(840, 580);

    AddSolid(1040, 650, 360, 100, SOLID_GROUND);
    AddSpike(1180, 630, 100, 20);
    AddCoin(1300, 580);

    AddAnchor(1520, 380);
    AddCoin(1520, 500);
    AddSolid(1650, 650, 320, 100, SOLID_GROUND);

    AddSolid(2050, 540, 150, 30, SOLID_FLOATING);
    AddCoin(2125, 490);
    AddSolid(2260, 420, 150, 30, SOLID_FLOATING);
    AddCoin(2335, 370);
    AddAnchor(2340, 200);

    AddSolid(2500, 650, 420, 100, SOLID_GROUND);
    AddSpike(2620, 630, 90, 20);
    AddSpike(2780, 630, 90, 20);
    AddCoin(2700, 580);

    AddSolid(2960, 380, 40, 370, SOLID_WALL);
    AddAnchor(2980, 220);
    AddCoin(2980, 300);

    AddSolid(3040, 650, 540, 100, SOLID_GROUND);
    AddSolid(3300, 250, 40, 300, SOLID_WALL);
    AddSolid(3540, 250, 40, 300, SOLID_WALL);
    AddAnchor(3440, 560);
    AddAnchor(3440, 430);
    AddAnchor(3440, 300);
    AddAnchor(3440, 170);
    AddCoin(3440, 480);
    AddCoin(3440, 350);
    AddCoin(3440, 220);
    AddSolid(3540, 150, 220, 30, SOLID_FLOATING);
    AddCoin(3650, 100);

    AddSolid(3760, 300, 150, 30, SOLID_FLOATING);
    AddSolid(3960, 440, 150, 30, SOLID_FLOATING);
    AddCoin(4035, 390);
    AddSolid(4160, 580, 150, 30, SOLID_FLOATING);
    AddSolid(4360, 650, 400, 100, SOLID_GROUND);
    AddSpike(4480, 630, 90, 20);
    AddSpike(4630, 630, 90, 20);

    AddAnchor(4910, 340);
    AddCoin(4910, 460);
    AddSolid(5060, 650, 800, 100, SOLID_GROUND);
    AddSpike(5280, 630, 90, 20);
    AddSpike(5440, 630, 90, 20);
    AddCoin(5360, 580);

    AddSolid(5610, 520, 160, 30, SOLID_FLOATING);
    AddCoin(5690, 470);
    AddAnchor(5690, 330);

    goalRect = (Rectangle){ 5760, 580, 40, 70 };

    spawnPoint = (Vector2){ 60, 650 - PLAYER_H };
    worldWidth = 6000.0f;
}

static void BuildLevel2(void)
{
    AddSolid(0, 650, 600, 100, SOLID_GROUND);
    AddCoin(300, 580);

    AddSolid(600, 650, 900, 100, SOLID_GROUND);
    AddSpike(780, 630, 90, 20);
    AddSpike(960, 630, 90, 20);
    AddSpike(1140, 630, 90, 20);
    AddSpike(1320, 630, 90, 20);
    AddCoin(870, 540);
    AddCoin(1050, 540);
    AddCoin(1230, 540);

    AddAnchor(1630, 380);
    AddCoin(1630, 500);
    AddSolid(1760, 650, 760, 100, SOLID_GROUND);
    AddSpike(1900, 630, 90, 20);
    AddSpike(2080, 630, 90, 20);
    AddCoin(1985, 560);

    AddSolid(2300, 480, 40, 170, SOLID_WALL);
    AddAnchor(2320, 300);
    AddCoin(2320, 400);

    AddAnchor(2620, 400);
    AddAnchor(2780, 380);
    AddCoin(2700, 470);
    AddSolid(2860, 650, 840, 100, SOLID_GROUND);
    AddSpike(3000, 630, 90, 20);
    AddSpike(3180, 630, 90, 20);
    AddSpike(3360, 630, 90, 20);
    AddCoin(3090, 540);
    AddCoin(3270, 540);

    AddAnchor(3830, 400);
    AddCoin(3830, 520);
    AddSolid(3950, 650, 450, 100, SOLID_GROUND);
    AddSpike(4080, 630, 90, 20);

    goalRect = (Rectangle){ 4300, 580, 40, 70 };
    spawnPoint = (Vector2){ 60, 650 - PLAYER_H };
    worldWidth = 4500.0f;
}

static void BuildLevel3(void)
{
    AddSolid(0, 650, 420, 100, SOLID_GROUND);
    AddCoin(200, 580);

    AddSolid(600, 600, 180, 30, SOLID_FLOATING);
    AddCoin(690, 550);
    AddSolid(960, 540, 180, 30, SOLID_FLOATING);
    AddCoin(1050, 490);
    AddSolid(1320, 470, 160, 30, SOLID_FLOATING);
    AddAnchor(1150, 250);
    AddCoin(1150, 330);

    AddAnchor(1630, 250);
    AddSolid(1780, 470, 160, 30, SOLID_FLOATING);
    AddCoin(1630, 350);

    AddSolid(2150, 400, 160, 30, SOLID_FLOATING);
    AddCoin(2230, 350);
    AddSolid(2500, 330, 160, 30, SOLID_FLOATING);
    AddSolid(2850, 260, 160, 30, SOLID_FLOATING);
    AddCoin(2930, 210);

    AddSolid(3150, 260, 260, 30, SOLID_FLOATING);
    AddSolid(3300, 100, 40, 160, SOLID_WALL);
    AddAnchor(3320, 20);
    AddCoin(3320, 60);

    AddSolid(3600, 300, 160, 30, SOLID_FLOATING);
    AddCoin(3680, 250);

    AddAnchor(3900, 140);
    AddAnchor(4080, 130);
    AddCoin(3990, 260);
    AddSolid(4200, 400, 180, 30, SOLID_FLOATING);

    AddSolid(4560, 470, 160, 30, SOLID_FLOATING);
    AddCoin(4640, 420);

    AddSolid(4900, 650, 700, 100, SOLID_GROUND);
    AddSpike(5100, 630, 90, 20);
    AddSpike(5280, 630, 90, 20);
    AddCoin(5190, 560);

    goalRect = (Rectangle){ 5500, 580, 40, 70 };
    spawnPoint = (Vector2){ 60, 650 - PLAYER_H };
    worldWidth = 5700.0f;
}

#define SECTION_WIDTH      760.0f
#define SECTION_WIDTH_ALT  680.0f

static void LowerFlatSpike(float x0)
{
    AddSolid(x0, 650, SECTION_WIDTH, 100, SOLID_GROUND);
    AddSpike(x0 + 300, 630, 100, 20);
    AddCoin(x0 + 140, 580);
    AddCoin(x0 + 620, 580);
}

static void LowerRunningJumpPit(float x0)
{
    float gapW = 200.0f;
    float g1 = 300.0f;
    AddSolid(x0, 650, g1, 100, SOLID_GROUND);
    AddSolid(x0 + g1 + gapW, 650, SECTION_WIDTH - g1 - gapW, 100, SOLID_GROUND);
    AddCoin(x0 + g1 + gapW * 0.5f, 560);
}

static void LowerHopWall(float x0)
{
    AddSolid(x0, 650, SECTION_WIDTH, 100, SOLID_GROUND);
    AddSolid(x0 + 340, 650 - 105, 28, 105, SOLID_WALL);
    AddSpike(x0 + 520, 630, 90, 20);
    AddCoin(x0 + 160, 580);
}

static void LowerSpikeGauntlet(float x0)
{
    float g1 = 480.0f;
    float gapW = 150.0f;
    AddSolid(x0, 650, g1, 100, SOLID_GROUND);
    AddSpike(x0 + 130, 630, 90, 20);
    AddSpike(x0 + 300, 630, 90, 20);
    AddSolid(x0 + g1 + gapW, 650, SECTION_WIDTH - g1 - gapW, 100, SOLID_GROUND);
    AddCoin(x0 + 60, 580);
}

static void LowerRaisedPlateau(float x0)
{
    float entryW = 150.0f;
    float plateauW = 320.0f;
    float plateauY = 650.0f - 95.0f;
    float exitW = SECTION_WIDTH - entryW - plateauW;

    AddSolid(x0, 650, entryW, 100, SOLID_GROUND);
    AddSolid(x0 + entryW, plateauY, plateauW, 750.0f - plateauY, SOLID_GROUND);
    AddEnemy(x0 + entryW + plateauW * 0.5f, plateauY,
        x0 + entryW + 25.0f, x0 + entryW + plateauW - 25.0f, 90.0f);
    AddCoin(x0 + entryW + plateauW * 0.5f, plateauY - 55.0f);
    AddSolid(x0 + entryW + plateauW, 650, exitW, 100, SOLID_GROUND);
}

static void LowerSunkenTrench(float x0)
{
    float entryW = 200.0f;
    float trenchW = 320.0f;
    float trenchY = 650.0f + 95.0f;
    float exitW = SECTION_WIDTH - entryW - trenchW;

    AddSolid(x0, 650, entryW, 100, SOLID_GROUND);
    AddSolid(x0 + entryW, trenchY, trenchW, 100, SOLID_GROUND);
    AddEnemy(x0 + entryW + trenchW * 0.5f, trenchY,
        x0 + entryW + 25.0f, x0 + entryW + trenchW - 25.0f, 100.0f);
    AddSolid(x0 + entryW + trenchW, 650, exitW, 100, SOLID_GROUND);
}

static void LowerStaircase(float x0)
{
    float cx = x0;
    float stepW = 110.0f;

    AddSolid(cx, 650, stepW, 100, SOLID_GROUND); cx += stepW;
    AddSolid(cx, 650 - 80, stepW, 180, SOLID_GROUND); cx += stepW;
    AddSolid(cx, 650 - 160, stepW, 260, SOLID_GROUND); cx += stepW;

    float plateauW = 140.0f;
    AddSolid(cx, 650 - 160, plateauW, 260, SOLID_GROUND);
    AddCoin(cx + plateauW * 0.5f, 650 - 160 - 50);
    cx += plateauW;

    AddSolid(cx, 650 - 80, stepW, 180, SOLID_GROUND); cx += stepW;

    float lastW = SECTION_WIDTH - (cx - x0);
    AddSolid(cx, 650, lastW, 100, SOLID_GROUND);
}

static void LowerEnemyGauntlet(float x0)
{
    AddSolid(x0, 650, SECTION_WIDTH, 100, SOLID_GROUND);
    AddEnemy(x0 + 200, 650, x0 + 110, x0 + 320, 85.0f);
    AddEnemy(x0 + 560, 650, x0 + 450, x0 + 690, 85.0f);
    AddCoin(x0 + 380, 580);
}

static void LowerTunnel(float x0)
{
    float entryW = 120.0f;
    float tunnelW = 380.0f;
    float exitW = SECTION_WIDTH - entryW - tunnelW;

    float clearance = 46.0f;
    float ceilBottomY = 650.0f - PLAYER_H - clearance;
    float ceilThickness = 90.0f;

    AddSolid(x0, 650, entryW, 100, SOLID_GROUND);
    AddSolid(x0 + entryW, 650, tunnelW, 100, SOLID_GROUND);
    AddSolid(x0 + entryW, ceilBottomY - ceilThickness, tunnelW, ceilThickness, SOLID_WALL);
    AddEnemy(x0 + entryW + tunnelW * 0.5f, 650, x0 + entryW + 30.0f, x0 + entryW + tunnelW - 30.0f, 110.0f);
    AddCoin(x0 + entryW + tunnelW * 0.2f, 650 - 30.0f);
    AddSolid(x0 + entryW + tunnelW, 650, exitW, 100, SOLID_GROUND);
}

typedef void (*LowerPatternFn)(float);
static LowerPatternFn lowerPatternsAct1[] = {
    LowerRaisedPlateau, LowerHopWall, LowerSunkenTrench, LowerSpikeGauntlet,
    LowerStaircase, LowerTunnel, LowerEnemyGauntlet, LowerRunningJumpPit,
    LowerRaisedPlateau, LowerSunkenTrench, LowerFlatSpike, LowerTunnel,
    LowerStaircase, LowerEnemyGauntlet, LowerHopWall, LowerRunningJumpPit
};
#define NUM_LOWER_PATTERNS_ACT1 16

static void Lower2_SpikeComb(float x0)
{
    AddSolid(x0, 690, SECTION_WIDTH_ALT, 100, SOLID_GROUND);
    for (int i = 0; i < 5; i++)
        AddSpike(x0 + 180 + i * 80, 670, 40, 20);
    AddCoin(x0 + SECTION_WIDTH_ALT * 0.5f, 600);
}

static void Lower2_RidgeAmbush(float x0)
{
    float entryW = 130.0f;
    float ridgeW = 300.0f;
    float ridgeY = 690.0f - 110.0f;
    float exitW = SECTION_WIDTH_ALT - entryW - ridgeW;

    AddSolid(x0, 690, entryW, 100, SOLID_GROUND);
    AddSolid(x0 + entryW, ridgeY, ridgeW, 790.0f - ridgeY, SOLID_GROUND);
    AddEnemy(x0 + entryW + ridgeW * 0.5f, ridgeY,
        x0 + entryW + 20.0f, x0 + entryW + ridgeW - 20.0f, 120.0f);
    AddSolid(x0 + entryW + ridgeW, 690, exitW, 100, SOLID_GROUND);
    AddSpike(x0 + entryW + ridgeW + 40, 670, 70, 20);
    AddCoin(x0 + entryW + ridgeW * 0.5f, ridgeY - 60.0f);
}

static void Lower2_CrawlSpikes(float x0)
{
    float entryW = 100.0f;
    float crawlW = 440.0f;
    float exitW = SECTION_WIDTH_ALT - entryW - crawlW;

    float clearance = 50.0f;
    float ceilBottomY = 690.0f - PLAYER_H - clearance;
    float ceilThickness = 80.0f;

    AddSolid(x0, 690, entryW, 100, SOLID_GROUND);
    AddSolid(x0 + entryW, 690, crawlW, 100, SOLID_GROUND);
    AddSolid(x0 + entryW, ceilBottomY - ceilThickness, crawlW, ceilThickness, SOLID_WALL);
    AddSpike(x0 + entryW + 90, 670, 70, 20);
    AddSpike(x0 + entryW + 280, 670, 70, 20);
    AddSolid(x0 + entryW + crawlW, 690, exitW, 100, SOLID_GROUND);
    AddCoin(x0 + entryW + crawlW - 60, 640);
}

static void Lower2_ChasmHop(float x0)
{
    float entryW = 160.0f;
    float chasmW = 360.0f;
    float exitW = SECTION_WIDTH_ALT - entryW - chasmW;

    AddSolid(x0, 690, entryW, 100, SOLID_GROUND);
    AddSolid(x0 + entryW, 690 + 160, chasmW, 100, SOLID_GROUND);
    AddSpike(x0 + entryW + 100, 670 + 160, 160, 20);
    AddSolid(x0 + entryW + chasmW * 0.5f - 40, 690 + 40, 80, 24, SOLID_FLOATING);
    AddCoin(x0 + entryW + chasmW * 0.5f, 690 + 10);
    AddSolid(x0 + entryW + chasmW, 690, exitW, 100, SOLID_GROUND);
}

static void Lower2_Descent(float x0)
{
    float cx = x0;
    float stepW = 130.0f;

    AddSolid(cx, 690, stepW, 100, SOLID_GROUND); cx += stepW;
    AddSolid(cx, 690 + 80, stepW, 180, SOLID_GROUND); cx += stepW;
    AddSolid(cx, 690 + 160, stepW, 260, SOLID_GROUND); cx += stepW;

    float shelfW = 130.0f;
    AddSolid(cx, 690 + 160, shelfW, 260, SOLID_GROUND);
    AddCoin(cx + shelfW * 0.5f, 690 + 160 - 50);
    cx += shelfW;

    AddSolid(cx, 690 + 80, stepW, 180, SOLID_GROUND); cx += stepW;

    float lastW = SECTION_WIDTH_ALT - (cx - x0);
    AddSolid(cx, 690, lastW, 100, SOLID_GROUND);
}

static void Lower2_EnemyPincer(float x0)
{
    float entryW = 140.0f;
    float corridorW = 400.0f;
    float exitW = SECTION_WIDTH_ALT - entryW - corridorW;

    AddSolid(x0, 690, entryW, 100, SOLID_GROUND);
    AddSolid(x0 + entryW, 690, corridorW, 100, SOLID_GROUND);
    AddEnemy(x0 + entryW + 110, 690, x0 + entryW + 30.0f,
        x0 + entryW + corridorW * 0.65f, 130.0f);
    AddEnemy(x0 + entryW + corridorW - 110, 690,
        x0 + entryW + corridorW * 0.35f,
        x0 + entryW + corridorW - 30.0f, 130.0f);
    AddSolid(x0 + entryW + corridorW, 690, exitW, 100, SOLID_GROUND);
    AddCoin(x0 + entryW + corridorW * 0.5f, 620);
}

static void Lower2_SteppingPit(float x0)
{
    float g1 = 220.0f;
    float pitW = 380.0f;
    float g2 = SECTION_WIDTH_ALT - g1 - pitW;

    AddSolid(x0, 690, g1, 100, SOLID_GROUND);
    AddSolid(x0 + g1 + pitW, 690, g2, 100, SOLID_GROUND);
    AddSolid(x0 + g1 + pitW * 0.33f - 40, 690 - 30, 80, 22, SOLID_FLOATING);
    AddSolid(x0 + g1 + pitW * 0.66f - 40, 690 - 30, 80, 22, SOLID_FLOATING);
    AddCoin(x0 + g1 + pitW * 0.5f, 610);
}

static LowerPatternFn lowerPatternsAct2[] = {
    Lower2_SpikeComb, Lower2_RidgeAmbush, Lower2_CrawlSpikes, Lower2_ChasmHop,
    Lower2_Descent, Lower2_EnemyPincer, Lower2_SteppingPit, Lower2_RidgeAmbush,
    Lower2_ChasmHop, Lower2_SpikeComb, Lower2_EnemyPincer, Lower2_Descent,
    Lower2_SteppingPit, Lower2_CrawlSpikes, Lower2_RidgeAmbush, Lower2_Descent
};
#define NUM_LOWER_PATTERNS_ACT2 16

static void UpperSingleSwing(float x0)
{
    AddAnchor(x0 + 380, 300);
    AddCoin(x0 + 380, 420);
}

static void UpperSwingToRest(float x0)
{
    AddAnchor(x0 + 190, 280);
    AddSolid(x0 + 420, 340, 160, 30, SOLID_FLOATING);
    AddCoin(x0 + 500, 290);
}

static void UpperClimbingChain(float x0)
{
    AddAnchor(x0 + 150, 350);
    AddAnchor(x0 + 500, 200);
    AddCoin(x0 + 500, 130);
}

typedef void (*UpperPatternFn)(float);
static UpperPatternFn upperPatternsAct1[] = { UpperSingleSwing, UpperSwingToRest, UpperClimbingChain };
#define NUM_UPPER_PATTERNS_ACT1 3

static void Upper2_DoubleSwing(float x0)
{
    AddAnchor(x0 + 200, 320);
    AddAnchor(x0 + 460, 240);
    AddCoin(x0 + 330, 380);
}

static void Upper2_VerticalLadder(float x0)
{
    AddAnchor(x0 + 360, 380);
    AddAnchor(x0 + 360, 270);
    AddAnchor(x0 + 360, 160);
    AddCoin(x0 + 360, 90);
}

static void Upper2_OffsetLedge(float x0)
{
    AddAnchor(x0 + 220, 290);
    AddSolid(x0 + 480, 360, 110, 22, SOLID_FLOATING);
    AddCoin(x0 + 520, 310);
}

static void Upper2_UnderSwing(float x0)
{
    AddAnchor(x0 + 340, 180);
    AddCoin(x0 + 340, 430);
}

static UpperPatternFn upperPatternsAct2[] = {
    Upper2_DoubleSwing, Upper2_VerticalLadder, Upper2_OffsetLedge,
    Upper2_UnderSwing, Upper2_VerticalLadder, Upper2_DoubleSwing
};
#define NUM_UPPER_PATTERNS_ACT2 6

static void BuildTwinLaneStretchAct1(float startX, int numSections, int lowerOffset, int upperOffset)
{
    for (int s = 0; s < numSections; s++)
    {
        float bx = startX + s * SECTION_WIDTH;
        lowerPatternsAct1[(s + lowerOffset) % NUM_LOWER_PATTERNS_ACT1](bx);
        upperPatternsAct1[(s + upperOffset) % NUM_UPPER_PATTERNS_ACT1](bx);
    }
}

static void BuildTwinLaneStretchAct2(float startX, int numSections, int lowerOffset, int upperOffset)
{
    for (int s = 0; s < numSections; s++)
    {
        float bx = startX + s * SECTION_WIDTH_ALT;
        lowerPatternsAct2[(s + lowerOffset) % NUM_LOWER_PATTERNS_ACT2](bx);
        upperPatternsAct2[(s + upperOffset) % NUM_UPPER_PATTERNS_ACT2](bx);
    }
}

static float Act2StretchWidth(int numSections)
{
    return numSections * SECTION_WIDTH_ALT;
}

static void BuildLevel4(void)
{
    AddSolid(0, 650, 520, 100, SOLID_GROUND);
    AddCoin(260, 580);
    AddSolid(660, 650, 240, 100, SOLID_GROUND);
    AddCoin(780, 580);

    float forkX = 900.0f;
    AddSolid(forkX - 60, 520, 120, 30, SOLID_FLOATING);
    AddAnchor(forkX + 60, 340);
    AddCoin(forkX - 20, 470);

    const int N1 = 10;
    BuildTwinLaneStretchAct1(forkX, N1, 0, 0);
    float towerX = forkX + N1 * SECTION_WIDTH;

    AddSolid(towerX, 650, 540, 100, SOLID_GROUND);
    AddSolid(towerX + 260, 250, 40, 300, SOLID_WALL);
    AddSolid(towerX + 500, 250, 40, 300, SOLID_WALL);
    AddAnchor(towerX + 380, 560);
    AddAnchor(towerX + 380, 430);
    AddAnchor(towerX + 380, 300);
    AddAnchor(towerX + 380, 170);
    AddCoin(towerX + 380, 480);
    AddCoin(towerX + 380, 350);
    AddCoin(towerX + 380, 220);
    AddSolid(towerX + 500, 150, 220, 30, SOLID_FLOATING);
    AddCoin(towerX + 610, 100);

    float descentX = towerX + SECTION_WIDTH;
    AddSolid(descentX, 300, 150, 30, SOLID_FLOATING);
    AddSolid(descentX + 220, 440, 150, 30, SOLID_FLOATING);
    AddCoin(descentX + 295, 390);
    AddSolid(descentX + 420, 580, 150, 30, SOLID_FLOATING);
    AddSolid(descentX + 620, 650, 300, 100, SOLID_GROUND);
    AddSpike(descentX + 700, 630, 90, 20);

    float fork2X = descentX + 1000.0f;
    AddSolid(fork2X - 80, 650 - 80, 160, 180, SOLID_GROUND);
    AddCoin(fork2X + 40, 580);
    AddAnchor(fork2X + 160, 420);

    const int N2 = 9;
    float act2Start = fork2X + 200.0f;
    BuildTwinLaneStretchAct2(act2Start, N2, 0, 0);
    float finalX = act2Start + Act2StretchWidth(N2);

    AddSolid(finalX, 690, 260, 100, SOLID_GROUND);
    AddSolid(finalX + 260, 650, 540, 100, SOLID_GROUND);
    AddSpike(finalX + 380, 630, 90, 20);
    AddSpike(finalX + 580, 630, 90, 20);
    AddCoin(finalX + 720, 580);

    AddSolid(finalX + 900, 520, 160, 30, SOLID_FLOATING);
    AddCoin(finalX + 980, 470);
    AddAnchor(finalX + 980, 330);

    goalRect = (Rectangle){ finalX + 1200, 580, 40, 70 };
    worldWidth = finalX + 1200 + 300;

    spawnPoint = (Vector2){ 60, 650 - PLAYER_H };
}

static void BuildLevel5(void)
{
    AddSolid(0, 650, 700, 100, SOLID_GROUND);
    AddCoin(180, 580);
    AddCoin(520, 580);
    AddAnchor(400, 380);
    AddSolid(720, 590, 130, 22, SOLID_FLOATING);
    AddSolid(900, 520, 130, 22, SOLID_FLOATING);
    AddCoin(810, 540);
    AddCoin(965, 470);
    AddMovingPlatform(1080, 470, 120, 22, 0, -260, 90.0f, 0.0f);

    AddSolid(1280, 180, 220, 30, SOLID_FLOATING);
    AddCoin(1390, 130);
    AddBreakablePlatform(1540, 180, 90, 20, 0.45f);
    AddBreakablePlatform(1670, 180, 90, 20, 0.45f);
    AddBreakablePlatform(1800, 180, 90, 20, 0.45f);
    AddCoin(1585, 130);
    AddCoin(1715, 130);
    AddCoin(1845, 130);
    AddSolid(1940, 180, 40, 260, SOLID_WALL);
    AddAnchor(1960, 60);
    AddSolid(2040, 200, 240, 30, SOLID_FLOATING);
    AddCoin(2160, 150);

    AddPhasingPlatform(2320, -120, 110, 22, 1.3f, 0.9f, 0.0f);
    AddPhasingPlatform(2470, -160, 110, 22, 1.3f, 0.9f, 1.1f);
    AddPhasingPlatform(2620, -120, 110, 22, 1.3f, 0.9f, 2.2f);
    AddCoin(2375, -170);
    AddCoin(2525, -210);
    AddCoin(2675, -170);
    AddSolid(2820, -180, 200, 30, SOLID_FLOATING);
    AddCoin(2920, -230);
    AddAnchor(2900, -360);

    AddBreakablePlatform(2840, -420, 90, 20, 0.40f);
    AddBreakablePlatform(2700, -520, 90, 20, 0.40f);
    AddBreakablePlatform(2560, -420, 90, 20, 0.40f);
    AddBreakablePlatform(2420, -520, 90, 20, 0.40f);
    AddBreakablePlatform(2280, -420, 90, 20, 0.40f);
    AddCoin(2885, -470);
    AddCoin(2745, -570);
    AddCoin(2605, -470);
    AddCoin(2465, -570);
    AddCoin(2325, -470);

    AddSolid(2140, -620, 900, 30, SOLID_GROUND);
    AddSpike(2500, -640, 80, 20);
    AddSpike(2700, -640, 80, 20);
    AddCoin(2400, -670);
    AddCoin(2600, -670);
    AddCoin(2800, -670);
    AddAnchor(3040, -900);
    AddSolid(3180, -800, 240, 30, SOLID_FLOATING);
    AddCoin(3300, -850);
    AddPhasingPlatform(3200, -720, 100, 22, 1.1f, 0.8f, 0.0f);
    AddPhasingPlatform(3350, -660, 100, 22, 1.1f, 0.8f, 0.95f);
    AddPhasingPlatform(3500, -720, 100, 22, 1.1f, 0.8f, 1.9f);
    AddCoin(3250, -770);
    AddCoin(3400, -710);
    AddCoin(3550, -770);

    AddMovingPlatform(3400, -1000, 140, 22, 0, -260, 110.0f, 0.0f);
    AddBreakablePlatform(3250, -1080, 90, 20, 0.40f);
    AddBreakablePlatform(3100, -1180, 90, 20, 0.40f);
    AddBreakablePlatform(3250, -1280, 90, 20, 0.40f);
    AddBreakablePlatform(3400, -1180, 90, 20, 0.40f);
    AddCoin(3305, -1130);
    AddCoin(3155, -1230);
    AddCoin(3305, -1330);
    AddCoin(3455, -1230);
    AddAnchor(3560, -1080);

    AddSolid(3560, -1480, 180, 30, SOLID_FLOATING);
    AddCoin(3650, -1530);
    AddMovingPlatform(3800, -1620, 120, 22, 180, -140, 95.0f, 0.0f);
    AddCoin(3900, -1700);
    AddSolid(4080, -1720, 180, 30, SOLID_FLOATING);
    AddCoin(4170, -1770);
    AddPhasingPlatform(4300, -1820, 110, 22, 1.4f, 0.9f, 0.0f);
    AddCoin(4355, -1870);

    AddSolid(4480, -1900, 320, 30, SOLID_FLOATING);
    AddSolid(4480, -2200, 30, 300, SOLID_WALL);
    AddSolid(4770, -2200, 30, 300, SOLID_WALL);
    AddAnchor(4620, -2150);
    AddCoin(4620, -1970);
    goalRect = (Rectangle){ 4600, -1970, 40, 70 };

    spawnPoint = (Vector2){ 100, 650 - PLAYER_H };
    worldWidth = 5200.0f;
    worldTopY = -2600.0f;
    worldBottomY = 900.0f;
}

static void SetLevelTheme(int index)
{
    switch (index)
    {
    case 0: currentBgTheme = BG_DEFAULT; currentPalette = paletteMeadow; break;
    case 1: currentBgTheme = BG_CAVERN;  currentPalette = paletteCavern; break;
    case 2: currentBgTheme = BG_SKY;     currentPalette = paletteSky;    break;
    case 3: currentBgTheme = BG_CITY;    currentPalette = paletteCity;   break;
    case 4: currentBgTheme = BG_TOWER;   currentPalette = paletteTower;  break;
    default: currentBgTheme = BG_EDITOR; currentPalette = paletteEditor; break;
    }
}

static void BuildLevel(int index)
{
    if (index == CUSTOM_LEVEL_INDEX)
    {
        return;
    }
    solidCount = spikeCount = anchorCount = coinCount = enemyCount = 0;
    worldTopY = -60.0f;
    worldBottomY = 1000.0f;
    SetLevelTheme(index);
    switch (index)
    {
    case 1:  BuildLevel2(); break;
    case 2:  BuildLevel3(); break;
    case 3:  BuildLevel4(); break;
    case 4:  BuildLevel5(); break;
    default: BuildLevel1(); break;
    }
}

static Rectangle PlayerRect(Vector2 pos)
{
    return (Rectangle) { pos.x, pos.y, PLAYER_W, PLAYER_H };
}

static Vector2 PlayerCenter(Player* p)
{
    return (Vector2) { p->position.x + PLAYER_W * 0.5f, p->position.y + PLAYER_H * 0.5f };
}

static bool IsSolidActive(Solid* s)
{
    if (s->type == SOLID_BREAKABLE && s->broken) return false;
    if (s->type == SOLID_PHASING && s->phasedOut) return false;
    return true;
}

static bool PlayerStandsOnSolid(Player* p, Solid* s)
{
    if (!p->onGround) return false;
    Rectangle pr = PlayerRect(p->position);
    Rectangle r = s->rect;
    bool horizOverlap = (pr.x + pr.width > r.x) && (pr.x < r.x + r.width);
    if (!horizOverlap) return false;
    float feetY = pr.y + pr.height;
    return fabsf(feetY - r.y) < 4.0f;
}

static int FindStandingBreakableIndex(Player* p)
{
    if (!p->onGround) return -1;
    for (int i = 0; i < solidCount; i++) {
        Solid* s = &solids[i];
        if (s->type != SOLID_BREAKABLE) continue;
        if (s->broken) continue;
        if (PlayerStandsOnSolid(p, s)) return i;
    }
    return -1;
}

static int FindStandingMovingIndex(Player* p)
{
    if (!p->onGround) return -1;
    for (int i = 0; i < solidCount; i++) {
        Solid* s = &solids[i];
        if (s->type != SOLID_MOVING) continue;
        if (!IsSolidActive(s)) continue;
        if (PlayerStandsOnSolid(p, s)) return i;
    }
    return -1;
}

static void UpdatePlatforms(Player* p, float dt)
{
    int ridingMoveIdx = FindStandingMovingIndex(p);
    int ridingBreakIdx = FindStandingBreakableIndex(p);
    Vector2 ridingDelta = { 0, 0 };

    for (int i = 0; i < solidCount; i++) {
        Solid* s = &solids[i];
        Vector2 oldPos = { s->rect.x, s->rect.y };

        switch (s->type) {
        case SOLID_MOVING: {
            float pathLen = Vector2Length(s->moveVec);
            if (pathLen > 1.0f) {
                s->movePhase += s->moveDir * (s->moveSpeed / pathLen) * dt;
                if (s->movePhase >= 1.0f) { s->movePhase = 1.0f; s->moveDir = -1.0f; }
                if (s->movePhase <= 0.0f) { s->movePhase = 0.0f; s->moveDir = 1.0f; }
                Vector2 np = Vector2Add(s->moveOrigin, Vector2Scale(s->moveVec, s->movePhase));
                s->rect.x = np.x;
                s->rect.y = np.y;
            }
            break;
        }
        case SOLID_BREAKABLE: {
            if (s->broken) {
                s->respawnTimer -= dt;
                if (s->respawnTimer <= 0.0f) {
                    s->broken = false;
                    s->breakTimer = 0.0f;
                    Vector2 c = { s->rect.x + s->rect.width * 0.5f,
                                  s->rect.y + s->rect.height * 0.5f };
                    SpawnBurst(c, 10, 140.0f, 0.4f, 3.0f, (Color) { 220, 190, 120, 255 });
                }
            }
            else if (i == ridingBreakIdx) {
                s->breakTimer += dt;
                if (s->breakTimer >= s->breakDelay) {
                    s->broken = true;
                    s->respawnTimer = 2.5f;
                    Vector2 c = { s->rect.x + s->rect.width * 0.5f,
                                  s->rect.y + s->rect.height * 0.5f };
                    SpawnBurst(c, 24, 240.0f, 0.6f, 4.0f, (Color) { 190, 120, 60, 255 });
                    Snd(sndLand);
                    Shake(3.0f, 0.10f);
                }
            }
            else {
                if (s->breakTimer > 0.0f) {
                    s->breakTimer -= dt * 2.5f;
                    if (s->breakTimer < 0.0f) s->breakTimer = 0.0f;
                }
            }
            break;
        }
        case SOLID_PHASING: {
            s->phaseTimer += dt;
            float cycle = s->phaseOnTime + s->phaseOffTime;
            if (cycle > 0.0f) {
                float t = fmodf(s->phaseTimer + s->phaseOffset, cycle);
                bool wasOut = s->phasedOut;
                s->phasedOut = (t >= s->phaseOnTime);
                if (wasOut && !s->phasedOut) {
                    Vector2 c = { s->rect.x + s->rect.width * 0.5f,
                                  s->rect.y + s->rect.height * 0.5f };
                    SpawnBurst(c, 8, 130.0f, 0.35f, 3.0f, (Color) { 190, 130, 250, 255 });
                }
            }
            break;
        }
        default: break;
        }

        Vector2 newPos = { s->rect.x, s->rect.y };
        s->velocity = (dt > 0.0f)
            ? Vector2Scale(Vector2Subtract(newPos, oldPos), 1.0f / dt)
            : (Vector2) { 0, 0 };

        if (i == ridingMoveIdx && s->type == SOLID_MOVING)
            ridingDelta = Vector2Subtract(newPos, oldPos);
    }

    if (ridingMoveIdx >= 0 && Vector2Length(ridingDelta) > 0.001f)
        p->position = Vector2Add(p->position, ridingDelta);
}

static void MoveAndCollide(Player* p, float dt)
{
    bool wasOnGround = p->onGround;

    p->position.x += p->velocity.x * dt;
    Rectangle pr = PlayerRect(p->position);
    for (int i = 0; i < solidCount; i++)
    {
        if (!IsSolidActive(&solids[i])) continue;
        if (CheckCollisionRecs(pr, solids[i].rect))
        {
            if (p->velocity.x > 0.0f) p->position.x = solids[i].rect.x - PLAYER_W;
            else if (p->velocity.x < 0.0f) p->position.x = solids[i].rect.x + solids[i].rect.width;

            if (solids[i].type == SOLID_WALL && fabsf(p->velocity.x) > WALL_BOUNCE_SPEED_THRESHOLD)
            {
                p->velocity.x = -p->velocity.x * WALL_BOUNCE_RESTITUTION;
                Snd(sndWallBounce);
                Shake(6.0f, 0.15f);
                SpawnBurst(PlayerCenter(p), 10, 220.0f, 0.35f, 4.0f, (Color) { 230, 230, 240, 255 });
            }
            else
            {
                p->velocity.x = 0.0f;
            }
            pr = PlayerRect(p->position);
        }
    }

    p->onGround = false;
    p->position.y += p->velocity.y * dt;
    pr = PlayerRect(p->position);
    for (int i = 0; i < solidCount; i++)
    {
        if (!IsSolidActive(&solids[i])) continue;
        if (CheckCollisionRecs(pr, solids[i].rect))
        {
            if (p->velocity.y > 0.0f)
            {
                p->position.y = solids[i].rect.y - PLAYER_H;
                p->onGround = true;
            }
            else if (p->velocity.y < 0.0f)
            {
                p->position.y = solids[i].rect.y + solids[i].rect.height;
            }
            p->velocity.y = 0.0f;
            pr = PlayerRect(p->position);
        }
    }

    if (p->onGround && !wasOnGround)
    {
        p->scale = (Vector2){ 1.35f, 0.65f };
        Snd(sndLand);
        Shake(3.0f, 0.08f);
        Vector2 feet = { p->position.x + PLAYER_W * 0.5f, p->position.y + PLAYER_H };
        SpawnBurst(feet, 8, 140.0f, 0.4f, 3.5f, (Color) { 210, 200, 170, 255 });
        p->usedDoubleJump = false;
        p->usedDash = false;
        p->coyoteTimer = 0.0f;
    }
}

static void ResolveSolidOverlap(Player* p)
{
    Rectangle pr = PlayerRect(p->position);
    for (int i = 0; i < solidCount; i++)
    {
        if (!IsSolidActive(&solids[i])) continue;
        Rectangle r = solids[i].rect;
        if (!CheckCollisionRecs(pr, r)) continue;

        Rectangle overlap = GetCollisionRec(pr, r);
        bool playerLeftOfCenter = (pr.x + pr.width * 0.5f) < (r.x + r.width * 0.5f);
        bool playerAboveCenter = (pr.y + pr.height * 0.5f) < (r.y + r.height * 0.5f);
        float pushX = playerLeftOfCenter ? -overlap.width : overlap.width;
        float pushY = playerAboveCenter ? -overlap.height : overlap.height;

        if (overlap.width < overlap.height)
        {
            p->position.x += pushX;
            if (solids[i].type == SOLID_WALL && fabsf(p->velocity.x) > WALL_BOUNCE_SPEED_THRESHOLD)
                p->velocity.x = -p->velocity.x * WALL_BOUNCE_RESTITUTION;
            else
                p->velocity.x = 0.0f;
        }
        else
        {
            p->position.y += pushY;
            if (pushY < 0.0f) p->onGround = true;
            p->velocity.y = 0.0f;
        }
        pr = PlayerRect(p->position);
    }
}

static bool TouchesAnySpike(Player* p)
{
    Rectangle pr = PlayerRect(p->position);
    for (int i = 0; i < spikeCount; i++)
        if (CheckCollisionRecs(pr, spikes[i])) return true;
    return false;
}

static bool TouchesAnyEnemy(Player* p)
{
    Rectangle pr = PlayerRect(p->position);
    for (int i = 0; i < enemyCount; i++)
        if (CheckCollisionRecs(pr, EnemyRect(&enemies[i]))) return true;
    return false;
}

static void CheckCoins(Player* p)
{
    Rectangle pr = PlayerRect(p->position);
    for (int i = 0; i < coinCount; i++)
    {
        if (coins[i].collected) continue;
        Rectangle cr = { coins[i].pos.x - 12, coins[i].pos.y - 12, 24, 24 };
        if (CheckCollisionRecs(pr, cr))
        {
            coins[i].collected = true;
            coinsCollected++;
            Snd(sndCoin);
            SpawnBurst(coins[i].pos, 12, 180.0f, 0.5f, 3.0f, (Color) { 255, 215, 60, 255 });
        }
    }
}

static void RespawnPlayer(Player* p)
{
    p->position = spawnPoint;
    p->velocity = (Vector2){ 0, 0 };
    p->grappling = false;
    p->onGround = false;
    p->coyoteTimer = 0.0f;
    p->jumpBufferTimer = 0.0f;
    p->usedDoubleJump = false;
    p->usedDash = false;
    p->dashTimer = 0.0f;
    p->scale = (Vector2){ 1.0f, 1.0f };
}

#define GHOST_HZ            60.0f
#define GHOST_MAX_SAMPLES   54000

typedef struct {
    Vector2 pos;
    Vector2 anchor;
    float facing;
    bool grappling;
} GhostSample;

typedef struct {
    GhostSample* samples;
    int count, cap;
} GhostTrack;

static GhostTrack recordingTrack;
static bool recordingOverflow = false;
static GhostTrack ghostTracks[LEVEL_COUNT];

static void RecordingReset(void)
{
    recordingTrack.count = 0;
    recordingOverflow = false;
}

static void RecordGhost(Player* p)
{
    int target = (int)(levelTime * GHOST_HZ);
    while (recordingTrack.count <= target)
    {
        if (recordingTrack.count >= GHOST_MAX_SAMPLES) { recordingOverflow = true; return; }
        if (recordingTrack.count >= recordingTrack.cap)
        {
            int newCap = recordingTrack.cap ? recordingTrack.cap * 2 : 2048;
            GhostSample* grown = realloc(recordingTrack.samples, newCap * sizeof(GhostSample));
            if (!grown) { recordingOverflow = true; return; }
            recordingTrack.samples = grown;
            recordingTrack.cap = newCap;
        }
        recordingTrack.samples[recordingTrack.count++] = (GhostSample){
            p->position, p->grappleAnchor, p->facing, p->grappling
        };
    }
}

static void SaveGhost(int level)
{
    if (recordingOverflow || recordingTrack.count < 2) return;
    GhostSample* copy = malloc(recordingTrack.count * sizeof(GhostSample));
    if (!copy) return;
    memcpy(copy, recordingTrack.samples, recordingTrack.count * sizeof(GhostSample));
    free(ghostTracks[level].samples);
    ghostTracks[level].samples = copy;
    ghostTracks[level].count = ghostTracks[level].cap = recordingTrack.count;
}

static bool GetGhostSample(int level, float t, GhostSample* out)
{
    GhostTrack* g = &ghostTracks[level];
    if (g->count < 2) return false;

    float f = t * GHOST_HZ;
    int i = (int)f;
    if (i >= g->count - 1)
    {
        *out = g->samples[g->count - 1];
    }
    else
    {
        GhostSample a = g->samples[i], b = g->samples[i + 1];
        float k = f - (float)i;
        *out = a;
        out->pos = Vector2Lerp(a.pos, b.pos, k);
        out->anchor = Vector2Lerp(a.anchor, b.anchor, k);
    }
    return true;
}

static void DrawGhost(int level, float t)
{
    GhostSample s;
    if (!GetGhostSample(level, t, &s)) return;

    Color body = (Color){ 130, 210, 255, 110 };
    Color edge = (Color){ 40, 110, 170, 150 };
    if (s.grappling)
        DrawLineEx((Vector2) { s.pos.x + PLAYER_W * 0.5f, s.pos.y + PLAYER_H * 0.5f }, s.anchor, 1.5f, (Color) { 130, 210, 255, 90 });
    Rectangle r = { s.pos.x, s.pos.y, PLAYER_W, PLAYER_H };
    DrawRectangleRec(r, body);
    DrawRectangleLinesEx(r, 2, edge);
    DrawCircle((int)(r.x + PLAYER_W * 0.5f + s.facing * 8.0f), (int)(r.y + 14), 3, (Color) { 255, 255, 255, 170 });
    const char* tag = "PB";
    DrawText(tag, (int)(r.x + PLAYER_W * 0.5f - MeasureText(tag, 12) * 0.5f), (int)r.y - 16, 12, (Color) { 130, 210, 255, 220 });
}

#define PREVIEW_W 456
#define PREVIEW_H 128
static RenderTexture2D levelPreviews[LEVEL_COUNT];

typedef struct { float sx, sy, minY, offY; } PreviewTransform;
static PreviewTransform previewTransform[LEVEL_COUNT];

// Themes a preview backdrop so the level-select cards read as a small slice
// of each level's world instead of a generic blue sky.
static void DrawPreviewBackdrop(int index)
{
    switch (index)
    {
    case 0: // meadow
        DrawRectangleGradientV(0, 0, PREVIEW_W, PREVIEW_H,
            (Color) {
            150, 200, 240, 255
        }, (Color) { 225, 240, 250, 255 });
        DrawCircle(PREVIEW_W - 60, 30, 26, (Color) { 255, 245, 210, 220 });
        for (int i = -1; i < 3; i++)
        {
            float bx = i * 200.0f + 40.0f;
            DrawCircle((int)bx, PREVIEW_H + 20, 90, (Color) { 170, 200, 175, 255 });
        }
        break;
    case 1: // cavern
        DrawRectangleGradientV(0, 0, PREVIEW_W, PREVIEW_H,
            (Color) {
            32, 14, 18, 255
        }, (Color) { 92, 38, 30, 255 });
        for (int i = -1; i < 5; i++)
        {
            float bx = i * 120.0f;
            DrawTriangle((Vector2) { bx, 0 }, (Vector2) { bx + 40, 0 }, (Vector2) { bx + 20, 60 }, (Color) { 52, 22, 22, 220 });
        }
        DrawCircle(PREVIEW_W / 2, PREVIEW_H + 40, 80, (Color) { 255, 130, 40, 40 });
        break;
    case 2: // sky
        DrawRectangleGradientV(0, 0, PREVIEW_W, PREVIEW_H,
            (Color) {
            90, 160, 230, 255
        }, (Color) { 215, 235, 250, 255 });
        DrawCircle(60, 30, 22, (Color) { 255, 250, 225, 235 });
        for (int i = 0; i < 3; i++)
        {
            float bx = 40.0f + i * 160.0f;
            float h = 40.0f + (i * 17 % 30);
            DrawRectangle((int)bx, (int)(PREVIEW_H - h), 34, (int)h, (Color) { 110, 140, 175, 150 });
        }
        for (int i = 0; i < 3; i++)
        {
            float cx = 30.0f + i * 170.0f;
            float cy = 24.0f + i * 24.0f;
            DrawCircle((int)cx, (int)cy, 14, (Color) { 255, 255, 255, 190 });
            DrawCircle((int)cx + 16, (int)cy + 4, 11, (Color) { 255, 255, 255, 190 });
        }
        break;
    case 3: // dusk city
        DrawRectangleGradientV(0, 0, PREVIEW_W, PREVIEW_H,
            (Color) {
            60, 40, 90, 255
        }, (Color) { 240, 150, 90, 255 });
        DrawCircle(PREVIEW_W - 70, PREVIEW_H - 30, 22, (Color) { 255, 220, 160, 230 });
        for (int i = 0; i < 5; i++)
        {
            float bx = 20.0f + i * 90.0f;
            float h = 30.0f + (i * 11 % 40);
            DrawRectangle((int)bx, (int)(PREVIEW_H - h), 44, (int)h, (Color) { 30, 26, 46, 220 });
            for (int wy = 0; wy < (int)(h / 14); wy++)
                DrawRectangle((int)bx + 6, (int)(PREVIEW_H - h + 6 + wy * 14), 4, 6, (Color) { 255, 200, 120, 180 });
        }
        break;
    case 4: // clocktower
        DrawRectangleGradientV(0, 0, PREVIEW_W, PREVIEW_H,
            (Color) {
            58, 36, 22, 255
        }, (Color) { 118, 78, 42, 255 });
        for (int i = 0; i < 4; i++)
            DrawRectangle(20 + i * 110, 0, 14, PREVIEW_H, (Color) { 40, 24, 14, 200 });
        DrawCircle(PREVIEW_W / 2, PREVIEW_H / 2, 46, (Color) { 180, 130, 70, 255 });
        DrawCircle(PREVIEW_W / 2, PREVIEW_H / 2, 40, (Color) { 30, 20, 14, 255 });
        for (int i = 0; i < 12; i++)
        {
            float a = 2.0f * PI * i / 12.0f;
            DrawLineEx(
                (Vector2) {
                PREVIEW_W / 2.0f + cosf(a) * 30, PREVIEW_H / 2.0f + sinf(a) * 30
            },
                (Vector2) {
                PREVIEW_W / 2.0f + cosf(a) * 38, PREVIEW_H / 2.0f + sinf(a) * 38
            },
                2, (Color) { 220, 190, 130, 255 });
        }
        break;
    default:
        DrawRectangleGradientV(0, 0, PREVIEW_W, PREVIEW_H,
            (Color) {
            150, 200, 240, 255
        }, (Color) { 225, 240, 250, 255 });
        break;
    }
}

static void BakeLevelPreview(int index)
{
    BuildLevel(index);

    float minY = goalRect.y;
    float maxY = goalRect.y + goalRect.height;
    for (int i = 0; i < solidCount; i++)
    {
        minY = fminf(minY, solids[i].rect.y);
        maxY = fmaxf(maxY, solids[i].rect.y + solids[i].rect.height);
    }
    for (int i = 0; i < anchorCount; i++) minY = fminf(minY, anchors[i].y);
    for (int i = 0; i < coinCount; i++) minY = fminf(minY, coins[i].pos.y);
    minY -= 40.0f;
    maxY += 40.0f;
    if (maxY < 720.0f) maxY = 720.0f;

    float sx = (float)PREVIEW_W / worldWidth;
    float sy = (float)PREVIEW_H / (maxY - minY);
    float offY = ((float)PREVIEW_H - (maxY - minY) * sy) * 0.5f;
    previewTransform[index] = (PreviewTransform){ sx, sy, minY, offY };
#define PV_X(wx) ((wx) * sx)
#define PV_Y(wy) (((wy) - minY) * sy + offY)

    levelPreviews[index] = LoadRenderTexture(PREVIEW_W, PREVIEW_H);
    SetTextureFilter(levelPreviews[index].texture, TEXTURE_FILTER_BILINEAR);

    BeginTextureMode(levelPreviews[index]);
    DrawPreviewBackdrop(index);

    for (int i = 0; i < solidCount; i++)
    {
        Color c;
        switch (solids[i].type)
        {
        case SOLID_FLOATING:  c = currentPalette.floating; break;
        case SOLID_WALL:      c = currentPalette.wall; break;
        case SOLID_MOVING:    c = currentPalette.moving; break;
        case SOLID_BREAKABLE: c = currentPalette.breakable; break;
        case SOLID_PHASING:   c = currentPalette.phasing; break;
        default:              c = currentPalette.ground; break;
        }
        Rectangle r = solids[i].rect;
        Rectangle pr = { PV_X(r.x), PV_Y(r.y), fmaxf(r.width * sx, 2.0f), fmaxf(r.height * sy, 2.0f) };
        DrawRectangleRec(pr, c);
    }
    for (int i = 0; i < spikeCount; i++)
    {
        Rectangle r = spikes[i];
        float w = fmaxf(r.width * sx, 3.0f);
        Vector2 base = { PV_X(r.x), PV_Y(r.y + r.height) };
        // Bright halo + solid fill so spikes read even on dark previews
        DrawTriangle((Vector2) { base.x - 1, base.y + 1 }, (Vector2) { base.x + w * 0.5f, base.y - 6.0f },
            (Vector2) {
            base.x + w + 1, base.y + 1
        }, (Color) { 255, 220, 120, 220 });
        DrawTriangle((Vector2) { base.x, base.y }, (Vector2) { base.x + w * 0.5f, base.y - 4.0f },
            (Vector2) {
            base.x + w, base.y
        }, (Color) { 200, 40, 40, 255 });
    }
    for (int i = 0; i < enemyCount; i++)
        DrawCircleV((Vector2) { PV_X(enemies[i].pos.x), PV_Y(enemies[i].pos.y) - 2.0f }, 2.4f, (Color) { 130, 40, 150, 255 });
    for (int i = 0; i < coinCount; i++)
        DrawCircleV((Vector2) { PV_X(coins[i].pos.x), PV_Y(coins[i].pos.y) }, 2.0f, (Color) { 255, 215, 60, 255 });
    for (int i = 0; i < anchorCount; i++)
        DrawCircleV((Vector2) { PV_X(anchors[i].x), PV_Y(anchors[i].y) }, 2.6f, (Color) { 90, 110, 170, 255 });
    DrawRectangle((int)PV_X(goalRect.x) - 2, (int)PV_Y(goalRect.y) - 4, 6, (int)fmaxf(goalRect.height * sy, 8.0f) + 4, (Color) { 60, 190, 90, 255 });
    DrawCircleV((Vector2) { PV_X(spawnPoint.x), PV_Y(spawnPoint.y) }, 3.4f, (Color) { 200, 60, 60, 255 });

    EndTextureMode();
#undef PV_X
#undef PV_Y
}

static Vector2 MinimapProject(PreviewTransform tr, Rectangle box, Vector2 worldPos)
{
    return (Vector2) {
        box.x + worldPos.x * tr.sx * (box.width / PREVIEW_W),
            box.y + (worldPos.y - tr.minY) * tr.sy * (box.height / PREVIEW_H) + tr.offY * (box.height / PREVIEW_H)
    };
}

static void DrawMinimap(int level, Vector2 playerCenter, float levelT, float realT)
{
    PreviewTransform tr = previewTransform[level];
    Rectangle box = { SCREEN_W - PREVIEW_W * 0.5f - 16, 92, PREVIEW_W * 0.5f, PREVIEW_H * 0.5f };

    DrawRectangleRec((Rectangle) { box.x - 4, box.y - 4, box.width + 8, box.height + 8 }, Fade(BLACK, 0.45f));
    DrawTexturePro(levelPreviews[level].texture,
        (Rectangle) {
        0, 0, (float)PREVIEW_W, -(float)PREVIEW_H
    },
        box, (Vector2) { 0, 0 }, 0.0f, WHITE);
    DrawRectangleLinesEx(box, 1.5f, (Color) { 230, 230, 235, 200 });

    GhostSample ghost;
    if (GetGhostSample(level, levelT, &ghost))
    {
        Vector2 gMark = MinimapProject(tr, box,
            (Vector2) {
            ghost.pos.x + PLAYER_W * 0.5f, ghost.pos.y + PLAYER_H * 0.5f
        });
        DrawCircleV(gMark, 3.0f, (Color) { 130, 210, 255, 230 });
        DrawCircleLines((int)gMark.x, (int)gMark.y, 3.0f, (Color) { 20, 60, 100, 220 });
    }

    Vector2 mark = MinimapProject(tr, box, playerCenter);
    float pulse = 0.7f + 0.3f * sinf(realT * 6.0f);
    DrawCircleV(mark, 5.0f * pulse, Fade((Color) { 255, 60, 60, 255 }, 0.35f));
    DrawCircleV(mark, 3.0f, (Color) { 255, 60, 60, 255 });
    DrawCircleLines((int)mark.x, (int)mark.y, 3.0f, (Color) { 255, 255, 255, 220 });
}

static void StartLevel(int index, Player* p)
{
    currentLevel = index;
    BuildLevel(index);
    RespawnPlayer(p);
    p->facing = 1.0f;
    coinsCollected = 0;
    levelTime = 0.0f;
    RecordingReset();
    cameraSmooth = PlayerCenter(p);
    speedIntensity = 0.0f;
    displaySpeed = 0.0f;
}

static bool SegmentHitsRect(Vector2 origin, Vector2 dir, float maxDist, Rectangle r)
{
    Vector2 end = Vector2Add(origin, Vector2Scale(dir, maxDist));

    if (CheckCollisionPointRec(origin, r) || CheckCollisionPointRec(end, r)) return true;

    Vector2 tl = { r.x, r.y };
    Vector2 tr = { r.x + r.width, r.y };
    Vector2 br = { r.x + r.width, r.y + r.height };
    Vector2 bl = { r.x, r.y + r.height };

    return CheckCollisionLines(origin, end, tl, tr, NULL)
        || CheckCollisionLines(origin, end, tr, br, NULL)
        || CheckCollisionLines(origin, end, br, bl, NULL)
        || CheckCollisionLines(origin, end, bl, tl, NULL);
}

static bool ReelBlockedByWall(Player* p)
{
    Vector2 center = PlayerCenter(p);
    Vector2 diff = Vector2Subtract(center, p->grappleAnchor);
    float dist = Vector2Length(diff);
    if (dist < 0.0001f) return false;
    Vector2 dir = Vector2Normalize(diff);

    Rectangle pr = PlayerRect(p->position);
    Rectangle touchRect = (Rectangle){ pr.x - 1, pr.y - 1, pr.width + 2, pr.height + 2 };

    for (int i = 0; i < solidCount; i++)
    {
        if (solids[i].type != SOLID_WALL) continue;
        if (!SegmentHitsRect(p->grappleAnchor, dir, dist, solids[i].rect)) continue;
        if (CheckCollisionRecs(touchRect, solids[i].rect)) return true;
    }
    return false;
}

static int FindBestAnchor(Player* p)
{
    Vector2 center = PlayerCenter(p);
    int best = -1;
    float bestDist = GRAPPLE_RANGE;
    for (int i = 0; i < anchorCount; i++)
    {
        float d = Vector2Distance(center, anchors[i]);
        if (d <= bestDist)
        {
            bestDist = d;
            best = i;
        }
    }
    return best;
}

static void TryFireGrapple(Player* p)
{
    Vector2 center = PlayerCenter(p);
    int best = FindBestAnchor(p);
    if (best >= 0)
    {
        p->grappling = true;
        p->grappleAnchor = anchors[best];
        p->ropeLength = fmaxf(Vector2Distance(center, anchors[best]), GRAPPLE_MIN_LEN);
        p->ropeDirValid = false;
        p->usedDoubleJump = false;
        p->usedDash = false;
        Snd(sndGrapple);
        SpawnBurst(anchors[best], 10, 160.0f, 0.4f, 3.0f, (Color) { 70, 220, 255, 255 });
    }
}

static void UpdateGrapple(Player* p, float dt)
{
    if (!p->grappling) return;

    if ((IsKeyDown(KEY_UP) || IsKeyDown(KEY_W)) && !ReelBlockedByWall(p))
    {
        p->ropeLength = Clamp(p->ropeLength - GRAPPLE_REEL_SPD * dt, GRAPPLE_MIN_LEN, GRAPPLE_RANGE);
    }
    if (IsKeyDown(KEY_DOWN) || IsKeyDown(KEY_S))
    {
        p->ropeLength = Clamp(p->ropeLength + GRAPPLE_REEL_SPD * dt, GRAPPLE_MIN_LEN, GRAPPLE_RANGE);
    }

    Vector2 center = PlayerCenter(p);
    Vector2 diff = Vector2Subtract(center, p->grappleAnchor);
    float dist = Vector2Length(diff);

    if (dist > p->ropeLength && dist > 0.0001f)
    {
        Vector2 dir = Vector2Normalize(diff);

        if (p->ropeDirValid)
            p->velocity = Vector2Rotate(p->velocity, Vector2Angle(p->ropeDir, dir));

        Vector2 newCenter = Vector2Add(p->grappleAnchor, Vector2Scale(dir, p->ropeLength));
        p->position.x = newCenter.x - PLAYER_W * 0.5f;
        p->position.y = newCenter.y - PLAYER_H * 0.5f;

        ResolveSolidOverlap(p);

        float radialSpeed = Vector2DotProduct(p->velocity, dir);
        if (radialSpeed > 0.0f)
        {
            p->velocity = Vector2Subtract(p->velocity, Vector2Scale(dir, radialSpeed));
        }

        Vector2 tangent = Vector2Rotate(dir, PI * 0.5f);
        float tangentSpeed = Vector2DotProduct(p->velocity, tangent);
        float sign = (tangentSpeed >= 0) ? 1.0f : -1.0f;
        p->velocity = Vector2Add(p->velocity, Vector2Scale(tangent, sign * GRAPPLE_PULL_ACC * dt * 0.15f));

        p->ropeDir = dir;
        p->ropeDirValid = true;
    }
    else
    {
        p->ropeDirValid = false;
    }

    if (GetRandomValue(0, 100) < 40)
    {
        float t = (float)GetRandomValue(20, 80) / 100.0f;
        Vector2 pt = Vector2Lerp(center, p->grappleAnchor, t);
        SpawnParticle(pt, (Vector2) { 0, -20 }, 0.25f, 2.0f, (Color) { 255, 240, 180, 200 });
    }
}

static void UpdateSpeedStreaks(float dt)
{
    for (int i = 0; i < MAX_STREAKS; i++)
    {
        if (!streaks[i].alive) continue;
        streaks[i].life -= dt;
        if (streaks[i].life <= 0.0f) { streaks[i].alive = false; continue; }
        streaks[i].pos.x -= streaks[i].speed * 0.02f * (streaks[i].color.r > 200 ? 1.0f : -1.0f);
    }
}

static void SpawnSpeedStreak(Vector2 playerVel, Vector2 screenCenter)
{
    if (speedIntensity < 0.15f) return;

    for (int i = 0; i < MAX_STREAKS; i++)
    {
        if (streaks[i].alive) continue;

        float angle = atan2f(-playerVel.y, -playerVel.x);
        float spread = (GetRandomValue(0, 100) / 100.0f - 0.5f) * 2.0f;
        float perpAngle = angle + PI * 0.5f + spread * 0.8f;

        float dist = 300.0f + (float)GetRandomValue(0, 200);

        streaks[i].pos.x = screenCenter.x + cosf(perpAngle) * dist + (float)GetRandomValue(-100, 100);
        streaks[i].pos.y = screenCenter.y + sinf(perpAngle) * dist + (float)GetRandomValue(-100, 100);
        streaks[i].speed = 400.0f + speedIntensity * 800.0f;
        streaks[i].maxLife = 0.15f + speedIntensity * 0.2f;
        streaks[i].life = streaks[i].maxLife;
        streaks[i].alive = true;

        unsigned char alpha = (unsigned char)(80 + speedIntensity * 175);
        streaks[i].color = (Color){ 255, 255, 255, alpha };
        return;
    }
}

static void DrawSpeedStreaks(Camera2D camera)
{
    (void)camera;

    for (int i = 0; i < MAX_STREAKS; i++)
    {
        if (!streaks[i].alive) continue;
        float t = streaks[i].life / streaks[i].maxLife;

        float length = 20.0f + speedIntensity * 80.0f;
        Color c = streaks[i].color;
        c.a = (unsigned char)(c.a * t);

        DrawLineEx(
            streaks[i].pos,
            (Vector2) {
            streaks[i].pos.x + length, streaks[i].pos.y
        },
            1.5f + speedIntensity * 1.5f,
            c
        );
    }
}

static void DrawRadialSpeedLines(Vector2 playerVel, float intensity)
{
    if (intensity < 0.05f) return;

    Vector2 center = { SCREEN_W / 2.0f, SCREEN_H / 2.0f };

    float moveAngle = atan2f(playerVel.y, playerVel.x);

    int numLines = (int)(30 * intensity);
    for (int i = 0; i < numLines; i++)
    {
        float a = moveAngle + PI + ((float)GetRandomValue(-100, 100) / 100.0f) * 0.9f;
        float edgeDist = 380.0f + (float)GetRandomValue(0, 180);
        float len = 40.0f + intensity * 120.0f;

        float sx = center.x + cosf(a) * edgeDist;
        float sy = center.y + sinf(a) * edgeDist;

        unsigned char alpha = (unsigned char)(30 + intensity * 90);
        Color c = (Color){ 255, 255, 255, alpha };

        DrawLineEx(
            (Vector2) {
            sx, sy
        },
            (Vector2) {
            sx - cosf(a) * len, sy - sinf(a) * len
        },
            1.0f + intensity * 2.0f,
            c
        );
    }
}

static void DrawSpeedVignette(float intensity)
{
    if (intensity < 0.1f) return;

    unsigned char alpha = (unsigned char)(intensity * 120);
    Color c = (Color){ 20, 10, 40, alpha };

    int bandSize = (int)(60 + intensity * 120);
    for (int i = 0; i < bandSize; i++)
    {
        float t = 1.0f - (float)i / bandSize;
        unsigned char a = (unsigned char)(alpha * t * t);
        Color cc = { c.r, c.g, c.b, a };

        DrawRectangle(0, i, SCREEN_W, 1, cc);
        DrawRectangle(0, SCREEN_H - i - 1, SCREEN_W, 1, cc);
        DrawRectangle(i, 0, 1, SCREEN_H, cc);
        DrawRectangle(SCREEN_W - i - 1, 0, 1, SCREEN_H, cc);
    }
}

static void DrawChromaticAberration(float intensity)
{
    if (intensity < 0.2f) return;

    float offset = intensity * 6.0f;
    unsigned char alpha = (unsigned char)(intensity * 70);

    Color redTint = { 255, 0, 0, alpha };
    Color blueTint = { 0, 80, 255, alpha };

    DrawRectangle(0, 0, (int)(offset * 3), SCREEN_H, redTint);
    DrawRectangle(SCREEN_W - (int)(offset * 3), 0, (int)(offset * 3), SCREEN_H, blueTint);
}

static void UpdateTrailGhosts(Player* p, float dt)
{
    for (int i = 0; i < MOTION_TRAIL_COUNT; i++)
    {
        if (trailGhosts[i].life > 0.0f)
            trailGhosts[i].life -= dt * 4.0f;
    }

    float speed = Vector2Length(p->velocity);
    if (speed > SPEED_BLUR_MIN)
    {
        trailTimer -= dt;
        if (trailTimer <= 0.0f)
        {
            trailTimer = 0.03f;

            for (int i = MOTION_TRAIL_COUNT - 1; i > 0; i--)
                trailGhosts[i] = trailGhosts[i - 1];

            trailGhosts[0].pos = p->position;
            trailGhosts[0].life = 1.0f;
            trailGhosts[0].scale = p->scale;
            trailGhosts[0].facing = p->facing;
            trailGhosts[0].grappling = p->grappling;
        }
    }
}

static void DrawTrailGhosts(void)
{
    for (int i = MOTION_TRAIL_COUNT - 1; i >= 0; i--)
    {
        if (trailGhosts[i].life <= 0.0f) continue;

        float t = trailGhosts[i].life;
        float w = PLAYER_W * trailGhosts[i].scale.x;
        float h = PLAYER_H * trailGhosts[i].scale.y;
        Rectangle draw = {
            trailGhosts[i].pos.x + (PLAYER_W - w) * 0.5f,
            trailGhosts[i].pos.y + (PLAYER_H - h),
            w, h
        };

        Color body = trailGhosts[i].grappling
            ? (Color) { 220, 130, 60, (unsigned char)(60 * t) }
        : (Color) { 200, 60, 60, (unsigned char)(60 * t) };

        DrawRectangleRec(draw, body);
    }
}

//------------------------------------------------------------------------------------
// Backgrounds
//------------------------------------------------------------------------------------
static void DrawBgDefault(Camera2D camera, float t)
{
    DrawRectangleGradientV(0, 0, SCREEN_W, SCREEN_H,
        (Color) {
        150, 200, 240, 255
    }, (Color) { 225, 240, 250, 255 });

    DrawCircleV((Vector2) { 180.0f - camera.target.x * 0.02f, 130.0f }, 46.0f, (Color) { 255, 245, 210, 220 });

    float p1 = camera.target.x * 0.15f;
    Color hillFar = (Color){ 170, 200, 175, 255 };
    for (int i = -1; i < 6; i++)
    {
        float bx = i * 500.0f - fmodf(p1, 500.0f);
        DrawCircle((int)bx + 150, SCREEN_H - 40, 220, hillFar);
    }

    float p2 = camera.target.x * 0.35f;
    Color hillNear = (Color){ 140, 185, 150, 255 };
    for (int i = -1; i < 6; i++)
    {
        float bx = i * 380.0f - fmodf(p2, 380.0f);
        DrawCircle((int)bx + 120, SCREEN_H + 10, 170, hillNear);
    }

    float p3 = camera.target.x * 0.08f + t * 12.0f;
    for (int i = -1; i < 5; i++)
    {
        float cx = i * 620.0f - fmodf(p3, 620.0f);
        float cy = 90.0f + 40.0f * sinf((float)i * 1.7f + t * 0.3f);
        Color cloud = (Color){ 255, 255, 255, 200 };
        DrawCircle((int)cx, (int)cy, 26, cloud);
        DrawCircle((int)cx + 28, (int)cy + 8, 20, cloud);
        DrawCircle((int)cx - 26, (int)cy + 10, 18, cloud);
    }
}

static void DrawBgCavern(Camera2D camera, float t)
{
    DrawRectangleGradientV(0, 0, SCREEN_W, SCREEN_H,
        (Color) {
        32, 14, 18, 255
    }, (Color) { 92, 38, 30, 255 });

    float pulse = 0.85f + 0.15f * sinf(t * 0.8f);
    for (int r = 260; r > 0; r -= 20)
    {
        unsigned char a = (unsigned char)(8 + (260 - r) * 0.15f * pulse);
        DrawCircleV((Vector2) { SCREEN_W * 0.5f, (float)SCREEN_H + 60.0f },
            (float)r, (Color) { 255, 120, 40, a });
    }

    float p1 = camera.target.x * 0.12f;
    Color spireFar = (Color){ 52,  22,  22, 220 };
    for (int i = -1; i < 8; i++)
    {
        float bx = i * 260.0f - fmodf(p1, 260.0f);
        float h = 220.0f + 90.0f * sinf((float)i * 1.9f);
        DrawTriangle((Vector2) { bx, 0.0f },
            (Vector2) {
            bx + 80.0f, 0.0f
        },
            (Vector2) {
            bx + 40.0f, h
        },
            spireFar);
        DrawTriangle((Vector2) { bx + 120.0f, (float)SCREEN_H },
            (Vector2) {
            bx + 220.0f, (float)SCREEN_H
        },
            (Vector2) {
            bx + 170.0f, (float)SCREEN_H - h * 0.7f
        },
            spireFar);
    }

    float p2 = camera.target.x * 0.24f;
    Color spireNear = (Color){ 24,  10,  12, 240 };
    for (int i = -1; i < 7; i++)
    {
        float bx = i * 360.0f - fmodf(p2, 360.0f) + 40.0f;
        float h = 300.0f + 120.0f * sinf((float)i * 2.3f + 0.7f);
        DrawTriangle((Vector2) { bx, 0.0f },
            (Vector2) {
            bx + 110.0f, 0.0f
        },
            (Vector2) {
            bx + 55.0f, h
        },
            spireNear);
    }

    for (int i = 0; i < 34; i++)
    {
        float seed = (float)i * 41.7f;
        float x = fmodf(seed * 47.0f + t * 8.0f, (float)SCREEN_W);
        float y = (float)SCREEN_H - fmodf(t * (12.0f + fmodf(seed, 20.0f)) + seed * 9.0f, (float)SCREEN_H);
        float tw = 0.5f + 0.5f * sinf(t * 3.0f + seed);
        unsigned char a = (unsigned char)(120 + tw * 120);
        DrawCircleV((Vector2) { x, y }, 1.4f + tw, (Color) { 255, 160, 70, a });
    }
}

#define CITY_MAX_BUILDINGS 40
typedef struct {
    float x;
    float w;
    float h;
    float parallax;
    unsigned char shade;
    bool  antenna;
} CityBuilding;

static CityBuilding cityBuildings[CITY_MAX_BUILDINGS];
static int          cityBuildingCount = 0;
static bool         cityBaked = false;

static void BakeCityBuildings(void)
{
    cityBuildingCount = 0;
    unsigned int seed = 0xC0FFEEu;

    for (int layer = 0; layer < 2; layer++)
    {
        float parallax = (layer == 0) ? 0.10f : 0.20f;
        float baseX = -200.0f + layer * 90.0f;
        float span = (layer == 0) ? 240.0f : 200.0f;

        for (int i = 0; i < 20 && cityBuildingCount < CITY_MAX_BUILDINGS; i++)
        {
            seed = seed * 1664525u + 1013904223u;
            float r1 = (float)((seed >> 8) & 0xFFFF) / 65535.0f;
            seed = seed * 1664525u + 1013904223u;
            float r2 = (float)((seed >> 8) & 0xFFFF) / 65535.0f;

            CityBuilding b = { 0 };
            b.parallax = parallax;
            b.x = baseX + i * span + r1 * 40.0f;

            float hBase = (layer == 0) ? 160.0f : 240.0f;
            float hVar = (layer == 0) ? 120.0f : 160.0f;
            b.h = hBase + r2 * hVar;

            b.w = (layer == 0) ? (70.0f + r1 * 30.0f) : (90.0f + r2 * 30.0f);
            b.shade = (unsigned char)(layer == 0 ? 90 : 60);
            b.antenna = (r1 > 0.55f);

            cityBuildings[cityBuildingCount++] = b;
        }
    }
    cityBaked = true;
}

static void DrawBgSky(Camera2D camera, float t)
{
    if (!cityBaked) BakeCityBuildings();

    DrawRectangleGradientV(0, 0, SCREEN_W, SCREEN_H,
        (Color) {
        90, 160, 230, 255
    }, (Color) { 215, 235, 250, 255 });

    {
        Vector2 sun = { 200.0f - camera.target.x * 0.03f, 140.0f };
        for (int r = 120; r > 0; r -= 12)
        {
            unsigned char a = (unsigned char)(8 + (120 - r) * 0.25f);
            DrawCircleV(sun, (float)r, (Color) { 255, 245, 210, a });
        }
        DrawCircleV(sun, 42.0f, (Color) { 255, 250, 225, 235 });
    }

    for (int i = 0; i < cityBuildingCount; i++)
    {
        CityBuilding* b = &cityBuildings[i];
        float screenX = b->x - camera.target.x * b->parallax;
        float wrapW = 4000.0f;
        screenX = fmodf(screenX, wrapW);
        if (screenX < -400.0f) screenX += wrapW;
        if (screenX > SCREEN_W + 400.0f) continue;

        Color c = (Color){ b->shade, (unsigned char)(b->shade + 30),
                           (unsigned char)(b->shade + 60), 200 };
        DrawRectangle((int)screenX, (int)(SCREEN_H - b->h), (int)b->w, (int)b->h, c);

        if (b->antenna)
            DrawRectangle((int)(screenX + b->w * 0.5f - 2),
                (int)(SCREEN_H - b->h - 20), 4, 20, c);
    }

    float pC1 = camera.target.x * 0.05f + t * 6.0f;
    for (int i = -1; i < 6; i++)
    {
        float cx = i * 520.0f - fmodf(pC1, 520.0f);
        float cy = 130.0f + 30.0f * sinf((float)i * 1.4f + t * 0.4f);
        Color c = (Color){ 255, 255, 255, 130 };
        DrawCircle((int)cx, (int)cy, 28, c);
        DrawCircle((int)cx + 34, (int)cy + 6, 22, c);
        DrawCircle((int)cx - 30, (int)cy + 8, 20, c);
    }

    float pC2 = camera.target.x * 0.12f + t * 14.0f;
    for (int i = -1; i < 5; i++)
    {
        float cx = i * 460.0f - fmodf(pC2, 460.0f);
        float cy = 260.0f + 45.0f * sinf((float)i * 1.9f + 1.1f + t * 0.5f);
        Color c = (Color){ 255, 255, 255, 190 };
        DrawCircle((int)cx, (int)cy, 36, c);
        DrawCircle((int)cx + 42, (int)cy + 10, 28, c);
        DrawCircle((int)cx - 40, (int)cy + 12, 26, c);
        DrawCircle((int)cx + 12, (int)cy - 18, 24, c);
    }

    float pC3 = camera.target.x * 0.22f + t * 26.0f;
    for (int i = -1; i < 4; i++)
    {
        float cx = i * 560.0f - fmodf(pC3, 560.0f);
        float cy = 480.0f + 55.0f * sinf((float)i * 2.2f + 0.4f + t * 0.6f);
        Color c = (Color){ 255, 255, 255, 235 };
        DrawCircle((int)cx, (int)cy, 48, c);
        DrawCircle((int)cx + 56, (int)cy + 14, 36, c);
        DrawCircle((int)cx - 52, (int)cy + 16, 34, c);
        DrawCircle((int)cx + 18, (int)cy - 24, 32, c);
    }
}

static void DrawBgCity(Camera2D camera, float t)
{
    if (!cityBaked) BakeCityBuildings();

    DrawRectangleGradientV(0, 0, SCREEN_W, SCREEN_H,
        (Color) {
        60, 40, 90, 255
    }, (Color) { 240, 150, 90, 255 });

    {
        Vector2 sun = { SCREEN_W * 0.7f - camera.target.x * 0.02f, SCREEN_H - 120.0f };
        for (int r = 200; r > 0; r -= 14)
        {
            unsigned char a = (unsigned char)(10 + (200 - r) * 0.10f);
            DrawCircleV(sun, (float)r, (Color) { 255, 200, 120, a });
        }
        DrawCircleV(sun, 70.0f, (Color) { 255, 220, 160, 230 });
    }

    for (int i = 0; i < cityBuildingCount; i++)
    {
        CityBuilding* b = &cityBuildings[i];
        float screenX = b->x - camera.target.x * b->parallax;
        float wrapW = 4000.0f;
        screenX = fmodf(screenX, wrapW);
        if (screenX < -400.0f) screenX += wrapW;
        if (screenX > SCREEN_W + 400.0f) continue;

        Color c = (Color){ 30, 26, 46, 220 };
        DrawRectangle((int)screenX, (int)(SCREEN_H - b->h), (int)b->w, (int)b->h, c);
        if (b->antenna)
            DrawRectangle((int)(screenX + b->w * 0.5f - 2),
                (int)(SCREEN_H - b->h - 20), 4, 20, c);

        int winCols = (int)(b->w / 16.0f);
        int winRows = (int)(b->h / 22.0f);
        for (int wy = 0; wy < winRows; wy++)
        {
            for (int wx = 0; wx < winCols; wx++)
            {
                unsigned int h = (unsigned int)(i * 131 + wx * 17 + wy * 7);
                if ((h & 3) == 0)
                {
                    float px = screenX + 6.0f + wx * 16.0f;
                    float py = SCREEN_H - b->h + 8.0f + wy * 22.0f;
                    unsigned char warm = (unsigned char)(180 + (h & 63));
                    float flick = 0.85f + 0.15f * sinf(t * 1.7f + (float)(h & 31));
                    DrawRectangle((int)px, (int)py, 6, 10,
                        (Color) {
                        255, warm, 120, (unsigned char)(220 * flick)
                    });
                }
            }
        }
    }

    float pS = camera.target.x * 0.08f + t * 22.0f;
    for (int i = -1; i < 6; i++)
    {
        float cx = i * 480.0f - fmodf(pS, 480.0f);
        float cy = SCREEN_H - 180.0f + 30.0f * sinf((float)i * 1.5f + t * 0.4f);
        Color c = (Color){ 200, 170, 200, 60 };
        DrawCircle((int)cx, (int)cy, 60, c);
        DrawCircle((int)cx + 70, (int)cy + 8, 50, c);
        DrawCircle((int)cx - 60, (int)cy + 10, 46, c);
    }
}

static void DrawGearRing(Vector2 center, float radius, float thickness,
    int teeth, float toothLen, float rot, Color color)
{
    DrawRing(center, radius, radius + thickness, 0.0f, 360.0f, 96, color);
    for (int i = 0; i < teeth; i++)
    {
        float a = rot + (2.0f * PI * (float)i) / (float)teeth;
        Vector2 inner = {
            center.x + cosf(a) * (radius + thickness),
            center.y + sinf(a) * (radius + thickness)
        };
        Vector2 outer = {
            center.x + cosf(a) * (radius + thickness + toothLen),
            center.y + sinf(a) * (radius + thickness + toothLen)
        };
        DrawLineEx(inner, outer, thickness * 1.1f, color);
    }
}

static void DrawBgTower(Camera2D camera, float t)
{
    DrawRectangleGradientV(0, 0, SCREEN_W, SCREEN_H,
        (Color) {
        58, 36, 22, 255
    },
        (Color) {
        118, 78, 42, 255
    });

    float pPillar = camera.target.x * 0.10f;
    Color pillar = (Color){ 40,  24,  14, 220 };
    Color pillarLit = (Color){ 150, 100,  50, 180 };
    for (int i = -1; i < 6; i++)
    {
        float bx = i * 340.0f - fmodf(pPillar, 340.0f);
        DrawRectangle((int)bx, 0, 34, SCREEN_H, pillar);
        DrawRectangle((int)bx + 8, 0, 3, SCREEN_H, pillarLit);
        for (int y = 20; y < SCREEN_H; y += 60)
            DrawCircle((int)bx + 17, y, 2.2f, (Color) { 200, 150, 80, 160 });
    }

    Vector2 clockCenter = {
        SCREEN_W * 0.5f - (camera.target.x - worldWidth * 0.5f) * 0.06f,
        SCREEN_H * 0.48f - (camera.target.y - (worldTopY + worldBottomY) * 0.5f) * 0.06f
    };

    DrawCircleV(clockCenter, 340.0f, (Color) { 46, 28, 16, 220 });
    DrawCircleV(clockCenter, 320.0f, (Color) { 180, 130, 70, 255 });
    DrawCircleV(clockCenter, 300.0f, (Color) { 30, 20, 14, 255 });
    DrawCircleV(clockCenter, 288.0f, (Color) { 220, 190, 130, 255 });
    DrawCircleV(clockCenter, 270.0f, (Color) { 46, 28, 16, 255 });
    DrawCircleV(clockCenter, 250.0f, (Color) { 108, 72, 40, 255 });

    for (int i = 0; i < 12; i++)
    {
        float a = (2.0f * PI * (float)i) / 12.0f - PI * 0.5f;
        Vector2 inner = { clockCenter.x + cosf(a) * 235.0f, clockCenter.y + sinf(a) * 235.0f };
        Vector2 outer = { clockCenter.x + cosf(a) * 268.0f, clockCenter.y + sinf(a) * 268.0f };
        DrawLineEx(inner, outer, 7.0f, (Color) { 230, 200, 140, 255 });
    }
    for (int i = 0; i < 60; i++)
    {
        if (i % 5 == 0) continue;
        float a = (2.0f * PI * (float)i) / 60.0f - PI * 0.5f;
        Vector2 p = { clockCenter.x + cosf(a) * 258.0f, clockCenter.y + sinf(a) * 258.0f };
        DrawCircleV(p, 1.8f, (Color) { 200, 170, 110, 200 });
    }

    DrawGearRing(clockCenter, 150.0f, 10.0f, 24, 12.0f, t * 0.35f, (Color) { 180, 130, 70, 255 });
    DrawGearRing(clockCenter, 88.0f, 10.0f, 16, 10.0f, -t * 0.55f, (Color) { 200, 150, 80, 255 });
    DrawGearRing(clockCenter, 40.0f, 8.0f, 10, 8.0f, t * 0.90f, (Color) { 220, 180, 110, 255 });

    DrawCircleV(clockCenter, 18.0f, (Color) { 60, 40, 22, 255 });
    DrawCircleV(clockCenter, 12.0f, (Color) { 230, 200, 140, 255 });

    Vector2 g1 = { 160.0f - camera.target.x * 0.04f, 180.0f + camera.target.y * 0.02f };
    Vector2 g2 = { SCREEN_W - 180.0f + camera.target.x * 0.03f, 220.0f - camera.target.y * 0.02f };
    DrawGearRing(g1, 70.0f, 8.0f, 16, 8.0f, t * 0.7f, (Color) { 150, 105, 60, 200 });
    DrawGearRing(g2, 90.0f, 9.0f, 20, 9.0f, -t * 0.5f, (Color) { 150, 105, 60, 200 });

    for (int i = 0; i < 40; i++)
    {
        float seed = (float)i * 27.31f;
        float x = fmodf(seed * 41.0f, (float)SCREEN_W);
        float y = SCREEN_H - fmodf(t * (25.0f + fmodf(seed, 40.0f)) + seed * 17.0f, (float)SCREEN_H);
        float tw = 0.5f + 0.5f * sinf(t * 2.5f + seed);
        unsigned char a = (unsigned char)(60 + tw * 120);
        DrawCircleV((Vector2) { x, y }, 1.6f + tw * 1.0f, (Color) { 255, 220, 150, a });
    }
}

static void DrawBackground(Camera2D camera, float time)
{
    switch (currentBgTheme)
    {
    case BG_CAVERN: DrawBgCavern(camera, time); break;
    case BG_SKY:    DrawBgSky(camera, time);    break;
    case BG_CITY:   DrawBgCity(camera, time);   break;
    case BG_TOWER:  DrawBgTower(camera, time);  break;
    default:        DrawBgDefault(camera, time); break;
    }
}

static bool IsButtonClicked(Rectangle rect)
{
    return CheckCollisionPointRec(GetMousePosition(), rect) && IsMouseButtonPressed(MOUSE_BUTTON_LEFT);
}

static void DrawButton(Rectangle rect, const char* label, int fontSize)
{
    bool hovered = CheckCollisionPointRec(GetMousePosition(), rect);
    Rectangle r = rect;
    if (hovered)
    {
        r.x -= 5; r.y -= 5; r.width += 10; r.height += 10;
    }

    Color border = (Color){ 40, 40, 60, 255 };
    Color bg = hovered ? (Color) { 250, 220, 120, 255 } : (Color) { 235, 235, 245, 255 };

    Rectangle shadow = { r.x + 3, r.y + 5, r.width, r.height };
    DrawRectangleRounded(shadow, 0.3f, 8, Fade(BLACK, 0.25f));
    DrawRectangleRounded(r, 0.3f, 8, bg);
    DrawRectangleRoundedLinesEx(r, 0.3f, 8, 2.0f, border);

    int tw = MeasureText(label, fontSize);
    DrawText(label, (int)(r.x + r.width * 0.5f - tw * 0.5f),
        (int)(r.y + r.height * 0.5f - fontSize * 0.5f), fontSize, border);
}

static void DrawLevelCard(Rectangle rect, int number, const char* name, float best, Texture2D preview)
{
    bool hovered = CheckCollisionPointRec(GetMousePosition(), rect);
    Rectangle r = rect;
    if (hovered)
    {
        r.x -= 6; r.y -= 6; r.width += 12; r.height += 12;
    }

    Color border = (Color){ 40, 40, 60, 255 };
    Color bg = hovered ? (Color) { 250, 220, 120, 255 } : (Color) { 235, 235, 245, 255 };

    DrawRectangleRounded((Rectangle) { r.x + 4, r.y + 6, r.width, r.height }, 0.12f, 8, Fade(BLACK, 0.25f));
    DrawRectangleRounded(r, 0.12f, 8, bg);
    DrawRectangleRoundedLinesEx(r, 0.12f, 8, 2.0f, border);

    char num[8];
    snprintf(num, sizeof(num), "%d", number);
    int nw = MeasureText(num, 56);
    DrawText(num, (int)(r.x + r.width * 0.5f - nw * 0.5f), (int)(r.y + 12), 56, border);

    // Title bar with a dark backing so it reads over both light sky previews
    // and the dark cavern / tower previews.
    const char* titleBar = name;
    int lw = MeasureText(titleBar, 24);
    DrawRectangle((int)(r.x + r.width * 0.5f - lw * 0.5f) - 6, (int)(r.y + 74) - 2,
        lw + 12, 28, Fade(BLACK, 0.35f));
    DrawText(titleBar, (int)(r.x + r.width * 0.5f - lw * 0.5f), (int)(r.y + 74), 24, RAYWHITE);

    Rectangle pv = { r.x + 16, r.y + 108, r.width - 32, (r.width - 32) * PREVIEW_H / PREVIEW_W };
    DrawTexturePro(preview, (Rectangle) { 0, 0, (float)preview.width, -(float)preview.height }, pv,
        (Vector2) {
        0, 0
    }, 0.0f, WHITE);
    DrawRectangleLinesEx(pv, 2.0f, border);

    char bestText[32];
    if (best > 0.0f) snprintf(bestText, sizeof(bestText), "Best: %05.2fs", best);
    else snprintf(bestText, sizeof(bestText), "Best: --");
    int bw = MeasureText(bestText, 18);
    DrawText(bestText, (int)(r.x + r.width * 0.5f - bw * 0.5f), (int)(r.y + r.height - 32), 18, (Color) { 90, 90, 110, 255 });
}

static void DrawTwinkleStars(float time)
{
    for (int i = 0; i < 40; i++)
    {
        float seed = (float)i * 37.13f;
        float x = fmodf(seed * 53.0f, (float)SCREEN_W);
        float y = fmodf(seed * 91.0f, (float)SCREEN_H);
        float twinkle = 0.5f + 0.5f * sinf(time * 2.0f + seed);
        DrawCircle((int)x, (int)y, 1.5f + twinkle * 1.5f, Fade(WHITE, 0.15f + twinkle * 0.35f));
    }
}

static void DrawSolid(Solid* s)
{
    if (s->type == SOLID_BREAKABLE && s->broken) return;

    bool phasedOff = (s->type == SOLID_PHASING && s->phasedOut);

    Color c;
    switch (s->type)
    {
    case SOLID_FLOATING:  c = currentPalette.floating; break;
    case SOLID_WALL:      c = currentPalette.wall; break;
    case SOLID_GROUND:    c = currentPalette.ground; break;
    case SOLID_MOVING:    c = currentPalette.moving; break;
    case SOLID_BREAKABLE: c = currentPalette.breakable; break;
    case SOLID_PHASING:   c = currentPalette.phasing; break;
    default:              c = GRAY; break;
    }

    if (phasedOff)
    {
        DrawRectangleLinesEx(s->rect, 2, Fade(c, 0.35f));
        return;
    }

    DrawRectangleRec(s->rect, c);
    DrawRectangleLinesEx(s->rect, 2, (Color) { 20, 20, 20, 255 });

    if (s->type == SOLID_GROUND)
    {
        DrawRectangle((int)s->rect.x, (int)s->rect.y, (int)s->rect.width, 8, currentPalette.groundTop);
    }

    if (s->type == SOLID_BREAKABLE)
    {
        float t = (s->breakDelay > 0.0f) ? (s->breakTimer / s->breakDelay) : 0.0f;
        if (t > 0.0f)
        {
            int numCracks = 1 + (int)(t * 4.0f);
            for (int i = 0; i < numCracks; i++)
            {
                float fx = s->rect.x + s->rect.width * (0.15f + 0.18f * i);
                DrawLine((int)fx, (int)s->rect.y,
                    (int)(fx + 6.0f), (int)(s->rect.y + s->rect.height),
                    (Color) {
                    60, 30, 10, (unsigned char)(120 + t * 135)
                });
            }
            if (t > 0.5f)
            {
                float pulse = 0.5f + 0.5f * sinf((float)GetTime() * 30.0f);
                unsigned char a = (unsigned char)((t - 0.5f) * 2.0f * 130 * pulse);
                DrawRectangleRec(s->rect, (Color) { 255, 60, 20, a });
            }
        }
    }

    if (s->type == SOLID_MOVING)
    {
        Vector2 pathEnd = Vector2Add(s->moveOrigin, s->moveVec);
        for (int k = 0; k <= 12; k++)
        {
            Vector2 pp = Vector2Lerp(s->moveOrigin, pathEnd, k / 12.0f);
            DrawCircleV((Vector2) {
                pp.x + s->rect.width * 0.5f,
                    pp.y + s->rect.height * 0.5f
            },
                1.5f, Fade(currentPalette.moving, 0.35f));
        }
        Vector2 center = { s->rect.x + s->rect.width * 0.5f,
                           s->rect.y + s->rect.height * 0.5f };
        Vector2 dir = Vector2Normalize(s->moveVec);
        if (Vector2Length(s->moveVec) > 0.1f)
        {
            Vector2 a = Vector2Add(center, Vector2Scale(dir, -10));
            Vector2 b = Vector2Add(center, Vector2Scale(dir, 10));
            DrawLineEx(a, b, 2, Fade((Color) { 20, 20, 20, 255 }, 0.7f));
        }
    }

    if (s->type == SOLID_PHASING)
    {
        float cycle = s->phaseOnTime + s->phaseOffTime;
        if (cycle > 0.0f)
        {
            float t = fmodf(s->phaseTimer + s->phaseOffset, cycle);
            float glow = (t < s->phaseOnTime)
                ? (0.6f + 0.4f * sinf((float)GetTime() * 6.0f))
                : 0.0f;
            if (glow > 0.0f)
                DrawRectangleRec(s->rect, Fade(WHITE, glow * 0.25f));
        }
    }
}

// Spikes: high-visibility hazard drawn in three layers.
//  - Bright cream/yellow halo silhouette (larger, always opaque)
//  - Opaque deep-red body with a warm gradient toward the tip
//  - Bright tip dot so the lethal point is obvious at a glance
static void DrawSpike(Rectangle r)
{
    int teeth = (int)(r.width / 20);
    if (teeth < 1) teeth = 1;
    float tw = r.width / teeth;

    for (int i = 0; i < teeth; i++)
    {
        Vector2 p1 = { r.x + i * tw,           r.y + r.height };
        Vector2 p2 = { r.x + (i + 0.5f) * tw,  r.y };
        Vector2 p3 = { r.x + (i + 1) * tw,     r.y + r.height };

        // Layer 1: bright halo behind the blade (offset outwards ~3px)
        Vector2 h1 = { p1.x - 3.0f, p1.y + 2.0f };
        Vector2 h2 = { p2.x,        p2.y - 4.0f };
        Vector2 h3 = { p3.x + 3.0f, p3.y + 2.0f };
        DrawTriangle(h1, h2, h3, (Color) { 255, 240, 180, 255 });

        // Layer 2: dark outline silhouette
        Vector2 o1 = { p1.x - 1.5f, p1.y + 1.0f };
        Vector2 o2 = { p2.x,        p2.y - 2.0f };
        Vector2 o3 = { p3.x + 1.5f, p3.y + 1.0f };
        DrawTriangle(o1, o2, o3, (Color) { 40, 4, 4, 255 });

        // Layer 3: main solid body (deep red)
        DrawTriangle(p1, p2, p3, (Color) { 190, 30, 30, 255 });

        // Warm gradient on the inner face (toward the tip)
        Vector2 midL = { (p1.x + p2.x) * 0.5f, (p1.y + p2.y) * 0.5f };
        Vector2 baseMid = { (p1.x + p3.x) * 0.5f, p1.y };
        DrawTriangle(p1, midL, baseMid, (Color) { 250, 100, 60, 255 });

        // Bright tip dot, so the point reads instantly
        DrawCircleV(p2, 2.2f, (Color) { 255, 250, 210, 255 });
        DrawCircleV(p2, 1.0f, (Color) { 255, 255, 255, 255 });
    }
}

// Dotted aim line: dark under-dot + bright over-dot for visibility on
// light and dark backgrounds alike.
static void DrawDottedLine(Vector2 a, Vector2 b, float time, Color color)
{
    const float spacing = 16.0f;
    float dist = Vector2Distance(a, b);
    if (dist < 1.0f) return;
    Vector2 dir = Vector2Scale(Vector2Subtract(b, a), 1.0f / dist);
    for (float d = fmodf(time * 40.0f, spacing); d < dist; d += spacing)
    {
        Vector2 p = Vector2Add(a, Vector2Scale(dir, d));
        DrawCircleV(p, 4.0f, (Color) { 0, 0, 0, 160 });
        DrawCircleV(p, 2.5f, color);
    }
}

static void DrawAnchor(Vector2 a, bool inRange)
{
    // Dark backing ring so anchors read on any background
    DrawCircleV(a, 15, (Color) { 0, 0, 0, 180 });
    Color c = inRange ? (Color) { 70, 220, 255, 255 } : (Color) { 90, 110, 170, 255 };
    DrawCircleV(a, 12, (Color) { 25, 35, 65, 255 });
    DrawRing(a, 9, 12, 0, 360, 24, c);
    DrawCircleV(a, 3.5f, c);
    DrawLineEx((Vector2) { a.x - 18, a.y }, (Vector2) { a.x - 8, a.y }, 2, c);
    DrawLineEx((Vector2) { a.x + 8, a.y }, (Vector2) { a.x + 18, a.y }, 2, c);
    DrawLineEx((Vector2) { a.x, a.y - 18 }, (Vector2) { a.x, a.y - 8 }, 2, c);
    DrawLineEx((Vector2) { a.x, a.y + 8 }, (Vector2) { a.x, a.y + 18 }, 2, c);
    DrawCircleLines((int)a.x, (int)a.y, (int)GRAPPLE_RANGE, (Color) { 90, 110, 170, 40 });
}

static void DrawCoin(Coin* c, float t)
{
    if (c->collected) return;
    float bobY = sinf(t * 3.0f + c->bob) * 5.0f;
    float squish = 0.55f + 0.45f * fabsf(cosf(t * 2.2f + c->bob));
    Vector2 pos = { c->pos.x, c->pos.y + bobY };
    // Dark halo makes the coin readable on light sky and white cloud backgrounds
    DrawEllipse((int)pos.x, (int)pos.y, 12.0f * squish, 12.0f, (Color) { 60, 40, 0, 200 });
    DrawEllipse((int)pos.x, (int)pos.y, 10.0f * squish, 10.0f, (Color) { 255, 215, 60, 255 });
    DrawEllipseLines((int)pos.x, (int)pos.y, 10.0f * squish, 10.0f, (Color) { 160, 120, 20, 255 });
    DrawEllipse((int)(pos.x - 2 * squish), (int)(pos.y - 3), 3.0f * squish, 3.0f, (Color) { 255, 250, 210, 220 });
}

static void DrawPlayer(Player* p)
{
    Rectangle pr = PlayerRect(p->position);
    Vector2 center = PlayerCenter(p);

    float w = PLAYER_W * p->scale.x;
    float h = PLAYER_H * p->scale.y;
    Rectangle draw = { center.x - w * 0.5f, center.y + PLAYER_H * 0.5f - h, w, h };

    // Dark outline behind the body so the player reads on light and dark bgs
    Rectangle outline = { draw.x - 2, draw.y - 2, draw.width + 4, draw.height + 4 };
    DrawRectangleRec(outline, (Color) { 0, 0, 0, 200 });

    Color body = p->grappling ? (Color) { 220, 130, 60, 255 } : (Color) { 200, 60, 60, 255 };
    DrawRectangleRec(draw, body);
    DrawRectangleLinesEx(draw, 2, (Color) { 30, 5, 5, 255 });

    float eyeX = draw.x + draw.width * 0.5f + p->facing * 8.0f;
    DrawCircle((int)eyeX, (int)(draw.y + draw.height * 0.32f), 4, BLACK);
    DrawCircle((int)eyeX, (int)(draw.y + draw.height * 0.32f), 3, WHITE);

    float speed = fabsf(p->velocity.x);
    if (speed > MAX_RUN_SPEED * 0.9f && p->onGround)
    {
        for (int i = 1; i <= 3; i++)
        {
            float a = 60 - i * 18;
            DrawLine((int)(pr.x - p->facing * (10 + i * 8)), (int)(pr.y + 10 + i * 6),
                (int)(pr.x - p->facing * (2 + i * 8)), (int)(pr.y + 10 + i * 6),
                Fade(WHITE, a / 255.0f));
        }
    }
}

// High-visibility grapple rope. Drawn as a bright yellow core with a thick
// dark outline so it stands out on every background (green hills, dark cave,
// bright sky, dusk city, brass tower).
static void DrawRope(Vector2 from, Vector2 to)
{
    DrawLineEx(from, to, 6.0f, (Color) { 0, 0, 0, 220 });        // outline
    DrawLineEx(from, to, 4.0f, (Color) { 60, 40, 10, 255 });     // inner dark
    DrawLineEx(from, to, 2.6f, (Color) { 255, 220, 90, 255 });   // bright core
    DrawLineEx(from, to, 1.0f, (Color) { 255, 255, 230, 255 });  // white highlight

    // Anchor end cap
    DrawCircleV(to, 6.0f, (Color) { 0, 0, 0, 220 });
    DrawCircleV(to, 4.5f, (Color) { 255, 220, 90, 255 });
    DrawCircleV(to, 2.0f, (Color) { 255, 255, 230, 255 });

    // Player end cap
    DrawCircleV(from, 5.0f, (Color) { 0, 0, 0, 220 });
    DrawCircleV(from, 3.5f, (Color) { 255, 220, 90, 255 });
    DrawCircleV(from, 1.5f, (Color) { 255, 255, 230, 255 });
}

static void DrawEditorGrid(Camera2D camera)
{
    Vector2 topLeft = GetScreenToWorld2D((Vector2) { 0, 0 }, camera);
    Vector2 bottomRight = GetScreenToWorld2D((Vector2) { SCREEN_W, SCREEN_H }, camera);
    float step = EDITOR_GRID * 4.0f;
    float startX = floorf(topLeft.x / step) * step;
    float startY = floorf(topLeft.y / step) * step;
    Color gridColor = (Color){ 255, 255, 255, 35 };

    for (float x = startX; x < bottomRight.x; x += step)
        DrawLine((int)x, (int)topLeft.y, (int)x, (int)bottomRight.y, gridColor);
    for (float y = startY; y < bottomRight.y; y += step)
        DrawLine((int)topLeft.x, (int)y, (int)bottomRight.x, (int)y, gridColor);
}

static void DrawEditorMarkers(void)
{
    DrawRectangleRec(goalRect, (Color) { 230, 230, 230, 255 });
    DrawRectangleLinesEx(goalRect, 2, (Color) { 40, 40, 60, 255 });
    DrawText("GOAL", (int)goalRect.x - 4, (int)goalRect.y - 22, 16, (Color) { 255, 230, 120, 255 });

    Rectangle sr = { spawnPoint.x, spawnPoint.y, PLAYER_W, PLAYER_H };
    DrawRectangleLinesEx(sr, 2, (Color) { 80, 220, 120, 255 });
    DrawText("SPAWN", (int)sr.x - 8, (int)sr.y - 22, 16, (Color) { 120, 255, 160, 255 });
}

static void EditorPlaceRect(EditorTool tool, Vector2 a, Vector2 b)
{
    float x0 = fminf(a.x, b.x), y0 = fminf(a.y, b.y);
    float w = fabsf(b.x - a.x), h = fabsf(b.y - a.y);
    bool isClick = (w < 12.0f && h < 12.0f);

    switch (tool)
    {
    case TOOL_GROUND:
        if (isClick) { w = 160.0f; h = 100.0f; }
        if (w < EDITOR_GRID) w = EDITOR_GRID;
        if (h < EDITOR_GRID) h = EDITOR_GRID;
        AddSolid(x0, y0, w, h, SOLID_GROUND);
        break;
    case TOOL_FLOATING:
        if (isClick) { w = 160.0f; h = 30.0f; }
        if (w < EDITOR_GRID) w = EDITOR_GRID;
        if (h < 12.0f) h = 12.0f;
        AddSolid(x0, y0, w, h, SOLID_FLOATING);
        break;
    case TOOL_WALL:
        if (isClick) { w = 30.0f; h = 110.0f; }
        if (w < 12.0f) w = 12.0f;
        if (h < EDITOR_GRID) h = EDITOR_GRID;
        AddSolid(x0, y0, w, h, SOLID_WALL);
        break;
    case TOOL_SPIKE:
        if (isClick) { w = 90.0f; h = 20.0f; }
        if (w < 20.0f) w = 20.0f;
        if (h < 10.0f) h = 10.0f;
        AddSpike(x0, y0, w, h);
        break;
    default: break;
    }
    RecalcCustomWorldBounds();
}

static void EditorPlaceEnemy(Vector2 a, Vector2 b)
{
    float minX = fminf(a.x, b.x), maxX = fmaxf(a.x, b.x);
    if (maxX - minX < 20.0f) { minX = a.x - 100.0f; maxX = a.x + 100.0f; }
    AddEnemy((minX + maxX) * 0.5f, a.y, minX, maxX, 90.0f);
    RecalcCustomWorldBounds();
}

static void RunEditor(Camera2D* camera, GameState* state, Player* player, bool* won, float dt)
{
    float panSpeed = 600.0f / editorZoom;
    if (IsKeyDown(KEY_RIGHT) || IsKeyDown(KEY_D)) editorCamTarget.x += panSpeed * dt;
    if (IsKeyDown(KEY_LEFT) || IsKeyDown(KEY_A)) editorCamTarget.x -= panSpeed * dt;
    if (IsKeyDown(KEY_DOWN) || IsKeyDown(KEY_S)) editorCamTarget.y += panSpeed * dt;
    if (IsKeyDown(KEY_UP) || IsKeyDown(KEY_W)) editorCamTarget.y -= panSpeed * dt;
    float wheel = GetMouseWheelMove();
    if (wheel != 0.0f) editorZoom = Clamp(editorZoom + wheel * 0.1f, 0.35f, 2.0f);

    camera->target = editorCamTarget;
    camera->zoom = editorZoom;
    camera->offset = (Vector2){ SCREEN_W / 2.0f, SCREEN_H / 2.0f };

    const int n = NUM_EDITOR_TOOLS;
    float btnW = 128.0f, btnH = 44.0f, gap = 6.0f;
    float totalW = n * btnW + (n - 1) * gap;
    float startX = (SCREEN_W - totalW) * 0.5f;
    Rectangle toolButtons[NUM_EDITOR_TOOLS];
    for (int i = 0; i < n; i++)
        toolButtons[i] = (Rectangle){ startX + i * (btnW + gap), 12, btnW, btnH };

    const char* actionLabels[5] = { "SAVE", "LOAD", "CLEAR", "PLAY (P)", "BACK" };
    float aBtnW = 170.0f, aBtnH = 44.0f, aGap = 10.0f;
    float aTotalW = 5 * aBtnW + 4 * aGap;
    float aStartX = (SCREEN_W - aTotalW) * 0.5f;
    Rectangle actionButtons[5];
    for (int i = 0; i < 5; i++)
        actionButtons[i] = (Rectangle){ aStartX + i * (aBtnW + aGap), 64, aBtnW, aBtnH };

    Vector2 mouse = GetMousePosition();
    Rectangle uiPanel = { 0, 0, SCREEN_W, 118 };
    bool overUI = CheckCollisionPointRec(mouse, uiPanel);

    for (int i = 0; i < n; i++)
        if (IsButtonClicked(toolButtons[i])) { editorTool = (EditorTool)i; Snd(sndCoin); }
    for (int i = 0; i < n; i++)
        if (IsKeyPressed(KEY_ONE + i)) editorTool = (EditorTool)i;

    if (IsButtonClicked(actionButtons[0])) SaveCustomLevel();
    if (IsButtonClicked(actionButtons[1]))
        EditorStatus(LoadCustomLevel() ? "Loaded!" : "No saved level found");
    if (IsButtonClicked(actionButtons[2]))
    {
        ResetCustomLevelScaffold();
        EditorStatus("Cleared");
    }
    if (IsButtonClicked(actionButtons[3]) || IsKeyPressed(KEY_P))
    {
        Snd(sndGrapple);
        StartLevel(CUSTOM_LEVEL_INDEX, player);
        *won = false;
        *state = STATE_PLAYING;
    }
    if (IsButtonClicked(actionButtons[4]) || IsKeyPressed(KEY_ESCAPE))
    {
        *state = STATE_LEVEL_SELECT;
    }

    Vector2 worldMouse = GetScreenToWorld2D(mouse, *camera);
    Vector2 snapped = { EditorSnap(worldMouse.x), EditorSnap(worldMouse.y) };

    if (!overUI)
    {
        if (IsMouseButtonPressed(MOUSE_BUTTON_LEFT))
        {
            if (editorTool == TOOL_ANCHOR)
            {
                AddAnchor(snapped.x, snapped.y);
                RecalcCustomWorldBounds();
                SpawnBurst(snapped, 8, 120.0f, 0.3f, 3.0f, (Color) { 250, 210, 60, 255 });
            }
            else if (editorTool == TOOL_COIN)
            {
                AddCoin(snapped.x, snapped.y);
                RecalcCustomWorldBounds();
                SpawnBurst(snapped, 8, 120.0f, 0.3f, 3.0f, (Color) { 255, 215, 60, 255 });
            }
            else if (editorTool == TOOL_GOAL)
            {
                goalRect = (Rectangle){ snapped.x - 20, snapped.y - 70, 40, 70 };
                RecalcCustomWorldBounds();
            }
            else if (editorTool == TOOL_SPAWN)
            {
                spawnPoint = (Vector2){ snapped.x - PLAYER_W * 0.5f, snapped.y - PLAYER_H };
                RecalcCustomWorldBounds();
            }
            else
            {
                editorDragging = true;
                editorDragStart = snapped;
            }
        }
        if (IsMouseButtonReleased(MOUSE_BUTTON_LEFT) && editorDragging)
        {
            editorDragging = false;
            if (editorTool == TOOL_ENEMY) EditorPlaceEnemy(editorDragStart, snapped);
            else EditorPlaceRect(editorTool, editorDragStart, snapped);
            Snd(sndLand);
            SpawnBurst(snapped, 10, 140.0f, 0.35f, 3.0f, (Color) { 200, 200, 220, 255 });
        }
        if (IsMouseButtonPressed(MOUSE_BUTTON_RIGHT))
        {
            if (TryEraseNear(worldMouse))
            {
                RecalcCustomWorldBounds();
                Snd(sndWallBounce);
                SpawnBurst(worldMouse, 10, 150.0f, 0.3f, 3.0f, (Color) { 220, 80, 80, 255 });
            }
        }
    }
    else if (IsMouseButtonReleased(MOUSE_BUTTON_LEFT))
    {
        editorDragging = false;
    }

    if (editorStatusTimer > 0.0f) editorStatusTimer -= dt;
    UpdateParticles(dt);

    BeginDrawing();
    ClearBackground((Color) { 190, 220, 245, 255 });
    DrawBackground(*camera, (float)GetTime());

    BeginMode2D(*camera);
    DrawEditorGrid(*camera);
    for (int i = 0; i < solidCount; i++) DrawSolid(&solids[i]);
    for (int i = 0; i < spikeCount; i++) DrawSpike(spikes[i]);
    DrawEnemies((float)GetTime());
    for (int i = 0; i < coinCount; i++) DrawCoin(&coins[i], (float)GetTime());
    for (int i = 0; i < anchorCount; i++) DrawAnchor(anchors[i], false);
    DrawEditorMarkers();

    if (editorDragging)
    {
        Rectangle preview = {
            fminf(editorDragStart.x, snapped.x), fminf(editorDragStart.y, snapped.y),
            fabsf(snapped.x - editorDragStart.x), fabsf(snapped.y - editorDragStart.y)
        };
        DrawRectangleRec(preview, Fade(SKYBLUE, 0.35f));
        DrawRectangleLinesEx(preview, 2, SKYBLUE);
    }
    if (!overUI) DrawCircleV(snapped, 4, Fade(WHITE, 0.8f));

    DrawParticles();
    EndMode2D();

    DrawRectangle(0, 0, SCREEN_W, 118, Fade(BLACK, 0.45f));
    for (int i = 0; i < n; i++)
    {
        DrawButton(toolButtons[i], editorToolLabels[i], 16);
        if ((int)editorTool == i)
            DrawRectangleLinesEx((Rectangle) {
            toolButtons[i].x - 3, toolButtons[i].y - 3,
                toolButtons[i].width + 6, toolButtons[i].height + 6
        },
                3, (Color) { 255, 230, 120, 255 });
    }
    for (int i = 0; i < 5; i++) DrawButton(actionButtons[i], actionLabels[i], 16);

    const char* hint = "Left-click/drag: place    Right-click: erase    WASD/Arrows: pan    Wheel: zoom    1-9: tools";
    int hw = MeasureText(hint, 16);
    DrawText(hint, SCREEN_W / 2 - hw / 2, 120, 16, (Color) { 230, 230, 235, 255 });

    if (editorStatusTimer > 0.0f)
    {
        unsigned char a = (unsigned char)(255 * Clamp(editorStatusTimer / 2.2f, 0.0f, 1.0f));
        int sw = MeasureText(editorStatusMsg, 20);
        DrawText(editorStatusMsg, SCREEN_W / 2 - sw / 2, 144, 20, (Color) { 255, 255, 255, a });
    }

    char counts[160];
    snprintf(counts, sizeof(counts), "Solids %d/%d   Spikes %d/%d   Anchors %d/%d   Coins %d/%d   Enemies %d/%d",
        solidCount, MAX_SOLIDS, spikeCount, MAX_SPIKES, anchorCount, MAX_ANCHORS,
        coinCount, MAX_COINS, enemyCount, MAX_ENEMIES);
    DrawText(counts, 12, SCREEN_H - 26, 16, (Color) { 230, 230, 235, 255 });

    EndDrawing();
}

int main(void)
{
    InitWindow(SCREEN_W, SCREEN_H, "AI Platformer - Momentum & Grapple");
    SetExitKey(KEY_NULL);
    SetTargetFPS(60);
    InitGameAudio();

    for (int i = 0; i < LEVEL_COUNT; i++) BakeLevelPreview(i);

    Player player = { 0 };
    StartLevel(0, &player);

    Camera2D camera = { 0 };
    camera.offset = (Vector2){ SCREEN_W / 2.0f, SCREEN_H / 2.0f };
    camera.zoom = 1.0f;

    bool won = false;
    bool quitRequested = false;
    GameState state = STATE_MENU;

    Rectangle playButton = { SCREEN_W / 2.0f - 110, SCREEN_H / 2.0f - 30, 220, 56 };
    Rectangle quitButton = { SCREEN_W / 2.0f - 110, SCREEN_H / 2.0f + 40, 220, 56 };
    float menuTime = 0.0f;
    bool playHoveredPrev = false;
    bool quitHoveredPrev = false;

    const float cardW = 200.0f, cardH = 200.0f, cardGap = 20.0f;
    const float cardsX = (SCREEN_W - (LEVEL_COUNT * cardW + (LEVEL_COUNT - 1) * cardGap)) / 2.0f;
    Rectangle levelCards[LEVEL_COUNT];
    for (int i = 0; i < LEVEL_COUNT; i++)
        levelCards[i] = (Rectangle){ cardsX + i * (cardW + cardGap), SCREEN_H / 2.0f - 90, cardW, cardH };
    Rectangle backButton = { SCREEN_W / 2.0f - 340, SCREEN_H / 2.0f + 160, 220, 56 };
    Rectangle createButton = { SCREEN_W / 2.0f - 100, SCREEN_H / 2.0f + 160, 220, 56 };
    int hoveredCardPrev = -1;
    bool backHoveredPrev = false;
    bool createHoveredPrev = false;

    while (!WindowShouldClose() && !quitRequested)
    {
        float dt = fminf(GetFrameTime(), 1.0f / 30.0f);

        if (state == STATE_MENU)
        {
            menuTime += dt;

            camera.target.x = menuTime * 35.0f;
            currentBgTheme = BG_DEFAULT;

            bool playHovered = CheckCollisionPointRec(GetMousePosition(), playButton);
            bool quitHovered = CheckCollisionPointRec(GetMousePosition(), quitButton);
            if ((playHovered && !playHoveredPrev) || (quitHovered && !quitHoveredPrev)) Snd(sndCoin);
            playHoveredPrev = playHovered;
            quitHoveredPrev = quitHovered;

            bool startClicked = IsButtonClicked(playButton) || IsKeyPressed(KEY_ENTER) || IsKeyPressed(KEY_SPACE);
            bool quitClicked = IsButtonClicked(quitButton);

            if (startClicked)
            {
                Snd(sndGrapple);
                Vector2 btnCenter = { playButton.x + playButton.width * 0.5f, playButton.y + playButton.height * 0.5f };
                SpawnBurst(btnCenter, 16, 200.0f, 0.5f, 3.5f, (Color) { 250, 220, 120, 255 });
                state = STATE_LEVEL_SELECT;
            }
            if (quitClicked) quitRequested = true;

            UpdateParticles(dt);

            BeginDrawing();
            ClearBackground((Color) { 190, 220, 245, 255 });
            DrawBackground(camera, menuTime);
            DrawTwinkleStars(menuTime);

            Rectangle panel = { SCREEN_W / 2.0f - 340, SCREEN_H / 2.0f - 220, 680, 420 };
            DrawRectangleRounded(panel, 0.08f, 12, Fade(BLACK, 0.22f));

            const char* title = "AI PLATFORMER";
            int fontSize = 64;
            int tw = MeasureText(title, fontSize);
            float titleY = SCREEN_H / 2.0f - 160.0f + sinf(menuTime * 1.6f) * 5.0f;
            DrawText(title, SCREEN_W / 2 - tw / 2 + 3, (int)titleY + 3, fontSize, Fade(BLACK, 0.35f));
            DrawText(title, SCREEN_W / 2 - tw / 2, (int)titleY, fontSize, (Color) { 255, 236, 160, 255 });

            DrawButton(playButton, "PLAY", 26);
            DrawButton(quitButton, "QUIT", 26);

            DrawParticles();

            const char* controls1 = "A/D or Arrows: run   SPACE: jump (double-jump in air!)   SHIFT: air dash   F / Click: grapple";
            const char* controls2 = "While grappling -> W/S or Up/Down: reel in/out    R: restart    ESC: level select";
            int cw1 = MeasureText(controls1, 16);
            int cw2 = MeasureText(controls2, 16);
            DrawText(controls1, SCREEN_W / 2 - cw1 / 2, SCREEN_H / 2 + 120, 16, (Color) { 230, 230, 235, 255 });
            DrawText(controls2, SCREEN_W / 2 - cw2 / 2, SCREEN_H / 2 + 144, 16, (Color) { 230, 230, 235, 255 });

            EndDrawing();
            continue;
        }

        if (state == STATE_LEVEL_SELECT)
        {
            menuTime += dt;
            camera.target.x = menuTime * 35.0f;
            currentBgTheme = BG_DEFAULT;

            int hoveredCard = -1;
            for (int i = 0; i < LEVEL_COUNT; i++)
                if (CheckCollisionPointRec(GetMousePosition(), levelCards[i])) hoveredCard = i;
            bool backHovered = CheckCollisionPointRec(GetMousePosition(), backButton);
            bool createHovered = CheckCollisionPointRec(GetMousePosition(), createButton);
            if ((hoveredCard >= 0 && hoveredCard != hoveredCardPrev) || (backHovered && !backHoveredPrev)
                || (createHovered && !createHoveredPrev)) Snd(sndCoin);
            hoveredCardPrev = hoveredCard;
            backHoveredPrev = backHovered;
            createHoveredPrev = createHovered;

            int chosen = -1;
            if (hoveredCard >= 0 && IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) chosen = hoveredCard;
            for (int i = 0; i < LEVEL_COUNT; i++)
                if (IsKeyPressed(KEY_ONE + i)) chosen = i;

            if (chosen >= 0)
            {
                Snd(sndGrapple);
                StartLevel(chosen, &player);
                won = false;
                state = STATE_PLAYING;
            }
            else if (IsButtonClicked(createButton))
            {
                Snd(sndGrapple);
                if (!LoadCustomLevel()) ResetCustomLevelScaffold();
                editorCamTarget = Vector2Add(spawnPoint, (Vector2) { 400.0f, -150.0f });
                editorZoom = 1.0f;
                editorDragging = false;
                editorStatusTimer = 0.0f;
                currentBgTheme = BG_EDITOR;
                currentPalette = paletteEditor;
                state = STATE_EDITOR;
            }
            else if (IsButtonClicked(backButton) || IsKeyPressed(KEY_ESCAPE))
            {
                state = STATE_MENU;
            }

            UpdateParticles(dt);

            BeginDrawing();
            ClearBackground((Color) { 190, 220, 245, 255 });
            DrawBackground(camera, menuTime);
            DrawTwinkleStars(menuTime);

            Rectangle panel = { SCREEN_W / 2.0f - 570, SCREEN_H / 2.0f - 250, 1140, 520 };
            DrawRectangleRounded(panel, 0.06f, 12, Fade(BLACK, 0.22f));

            const char* title = "SELECT LEVEL";
            int tw = MeasureText(title, 56);
            float titleY = SCREEN_H / 2.0f - 200.0f + sinf(menuTime * 1.6f) * 4.0f;
            DrawText(title, SCREEN_W / 2 - tw / 2 + 3, (int)titleY + 3, 56, Fade(BLACK, 0.35f));
            DrawText(title, SCREEN_W / 2 - tw / 2, (int)titleY, 56, (Color) { 255, 236, 160, 255 });

            for (int i = 0; i < LEVEL_COUNT; i++)
                DrawLevelCard(levelCards[i], i + 1, levelNames[i], bestTimes[i], levelPreviews[i].texture);
            DrawButton(backButton, "BACK", 26);
            DrawButton(createButton, "CREATE LEVEL", 20);

            char hint[96];
            snprintf(hint, sizeof(hint), "Click a level or press 1-%d    CREATE LEVEL: build your own    ESC: back", LEVEL_COUNT);
            int hw = MeasureText(hint, 16);
            DrawText(hint, SCREEN_W / 2 - hw / 2, SCREEN_H / 2 + 235, 16, (Color) { 230, 230, 235, 255 });

            DrawParticles();
            EndDrawing();
            continue;
        }

        if (state == STATE_EDITOR)
        {
            RunEditor(&camera, &state, &player, &won, dt);
            continue;
        }

        if (IsKeyPressed(KEY_ESCAPE))
        {
            state = (currentLevel == CUSTOM_LEVEL_INDEX) ? STATE_EDITOR : STATE_LEVEL_SELECT;
            hoveredCardPrev = -1;
        }

        if (IsKeyPressed(KEY_R))
        {
            StartLevel(currentLevel, &player);
            won = false;
        }

        if (!won)
        {
            levelTime += dt;

            float moveDir = 0.0f;
            if (IsKeyDown(KEY_RIGHT) || IsKeyDown(KEY_D)) moveDir += 1.0f;
            if (IsKeyDown(KEY_LEFT) || IsKeyDown(KEY_A)) moveDir -= 1.0f;
            if (moveDir != 0.0f) player.facing = moveDir;

            float accel = player.onGround ? GROUND_ACCEL : AIR_ACCEL;
            float friction = player.onGround ? GROUND_FRICTION : AIR_FRICTION;

            if (moveDir != 0.0f)
            {
                float vx = player.velocity.x;
                if (player.grappling)
                {
                    player.velocity.x += moveDir * accel * dt;
                }
                else if (fabsf(vx) <= MAX_RUN_SPEED)
                {
                    player.velocity.x = Clamp(vx + moveDir * accel * dt, -MAX_RUN_SPEED, MAX_RUN_SPEED);
                }
                else if (vx * moveDir < 0.0f)
                {
                    player.velocity.x += moveDir * accel * dt;
                }
                else if (player.onGround)
                {
                    player.velocity.x = copysignf(fmaxf(fabsf(vx) - friction * dt, MAX_RUN_SPEED), vx);
                }
                if (player.onGround && GetRandomValue(0, 100) < 12)
                {
                    Vector2 feet = { player.position.x + PLAYER_W * 0.5f, player.position.y + PLAYER_H };
                    SpawnParticle(feet, (Vector2) { -moveDir * 40.0f, -30.0f }, 0.3f, 2.5f, (Color) { 210, 200, 170, 200 });
                }
            }
            else if (!(player.grappling && !player.onGround))
            {
                if (player.velocity.x > 0.0f)
                {
                    player.velocity.x -= friction * dt;
                    if (player.velocity.x < 0.0f) player.velocity.x = 0.0f;
                }
                else if (player.velocity.x < 0.0f)
                {
                    player.velocity.x += friction * dt;
                    if (player.velocity.x > 0.0f) player.velocity.x = 0.0f;
                }
            }

            if (player.onGround) player.coyoteTimer = COYOTE_TIME;
            else player.coyoteTimer -= dt;

            if (IsKeyPressed(KEY_SPACE)) player.jumpBufferTimer = JUMP_BUFFER_TIME;
            else player.jumpBufferTimer -= dt;

            bool canGroundJump = (player.onGround || player.coyoteTimer > 0.0f);
            if (player.jumpBufferTimer > 0.0f && canGroundJump)
            {
                player.velocity.y = JUMP_VELOCITY;
                player.onGround = false;
                player.coyoteTimer = 0.0f;
                player.jumpBufferTimer = 0.0f;
                player.usedDoubleJump = false;
                player.scale = (Vector2){ 0.7f, 1.35f };
                Snd(sndJump);
                Vector2 feet = { player.position.x + PLAYER_W * 0.5f, player.position.y + PLAYER_H };
                SpawnBurst(feet, 6, 120.0f, 0.3f, 2.5f, (Color) { 210, 200, 170, 220 });
            }
            else if (IsKeyPressed(KEY_SPACE) && !canGroundJump && !player.grappling && !player.usedDoubleJump)
            {
                player.velocity.y = DOUBLE_JUMP_VELOCITY;
                player.usedDoubleJump = true;
                player.jumpBufferTimer = 0.0f;
                player.scale = (Vector2){ 0.75f, 1.3f };
                Snd(sndDoubleJump);
                Shake(2.0f, 0.06f);
                SpawnBurst(PlayerCenter(&player), 14, 160.0f, 0.35f, 3.0f, (Color) { 180, 220, 255, 255 });
            }

            if (IsKeyReleased(KEY_SPACE) && player.velocity.y < 0.0f && !player.grappling)
            {
                player.velocity.y *= JUMP_CUT_MULT;
            }

            if (IsKeyPressed(KEY_F) || IsMouseButtonPressed(MOUSE_BUTTON_LEFT))
            {
                if (player.grappling) player.grappling = false;
                else TryFireGrapple(&player);
            }

            // ---- Air dash: one burst per airtime, refreshed by grounding or grappling ----
            if (player.dashTimer > 0.0f) player.dashTimer -= dt;

            if ((IsKeyPressed(KEY_LEFT_SHIFT) || IsKeyPressed(KEY_RIGHT_SHIFT)) &&
                !player.onGround && !player.grappling && !player.usedDash)
            {
                player.dashDir = (moveDir != 0.0f) ? moveDir : player.facing;
                // Guarantee at least dash speed; never slow down a faster swing/fall.
                player.dashSpeed = fmaxf(fabsf(player.velocity.x), DASH_SPEED);
                player.velocity.x = player.dashDir * player.dashSpeed;
                player.velocity.y *= 0.2f; // flatten the arc into a burst
                player.dashTimer = DASH_DURATION;
                player.usedDash = true;
                player.scale = (Vector2){ 1.5f, 0.6f };
                Snd(sndDash);
                Shake(2.0f, 0.05f);
                SpawnBurst(PlayerCenter(&player), 12, 220.0f, 0.3f, 3.0f, (Color) { 255, 240, 120, 255 });
            }
            else if (player.dashTimer > 0.0f)
            {
                // Hold the burst speed for the rest of its duration, overriding
                // whatever the run/air accel above did this frame.
                player.velocity.x = player.dashDir * player.dashSpeed;
            }

            float gravityMult = (player.dashTimer > 0.0f) ? DASH_GRAVITY_MULT : 1.0f;
            player.velocity.y += GRAVITY * gravityMult * dt;
            if (!player.grappling) player.velocity.y = fminf(player.velocity.y, MAX_FALL_SPEED);

            UpdatePlatforms(&player, dt);
            ResolveSolidOverlap(&player);

            MoveAndCollide(&player, dt);
            UpdateGrapple(&player, dt);

            player.scale.x = Lerp(player.scale.x, 1.0f, 1.0f - expf(-14.0f * dt));
            player.scale.y = Lerp(player.scale.y, 1.0f, 1.0f - expf(-14.0f * dt));

            CheckCoins(&player);

            UpdateEnemies(dt);

            if (TouchesAnySpike(&player))
            {
                Snd(sndDeath);
                Shake(8.0f, 0.2f);
                SpawnBurst(PlayerCenter(&player), 16, 220.0f, 0.5f, 4.0f, (Color) { 220, 40, 40, 255 });
                StartLevel(currentLevel, &player);
            }
            if (TouchesAnyEnemy(&player))
            {
                Snd(sndDeath);
                Shake(8.0f, 0.2f);
                SpawnBurst(PlayerCenter(&player), 16, 220.0f, 0.5f, 4.0f, (Color) { 150, 50, 170, 255 });
                StartLevel(currentLevel, &player);
            }
            if (player.position.y > WORLD_DEATH_Y)
            {
                Snd(sndDeath);
                StartLevel(currentLevel, &player);
            }

            RecordGhost(&player);

            if (CheckCollisionRecs(PlayerRect(player.position), goalRect))
            {
                won = true;
                Snd(sndWin);
                Shake(10.0f, 0.3f);
                SpawnBurst(PlayerCenter(&player), 40, 260.0f, 0.8f, 4.5f, (Color) { 255, 220, 90, 255 });
                if (currentLevel != CUSTOM_LEVEL_INDEX &&
                    (bestTimes[currentLevel] < 0.0f || levelTime < bestTimes[currentLevel]))
                {
                    bestTimes[currentLevel] = levelTime;
                    SaveGhost(currentLevel);
                }
            }
        }

        UpdateParticles(dt);
        if (shakeTime > 0.0f) shakeTime -= dt; else shakeMagnitude = 0.0f;

        float rawSpeed = Vector2Length(player.velocity);
        float targetIntensity = (rawSpeed - SPEED_BLUR_MIN) / (SPEED_BLUR_MAX - SPEED_BLUR_MIN);
        if (targetIntensity < 0.0f) targetIntensity = 0.0f;
        if (targetIntensity > 1.0f) targetIntensity = 1.0f;
        float horizSpeed = fabsf(player.velocity.x);
        float horizIntensity = (horizSpeed - 200.0f) / 500.0f;
        if (horizIntensity < 0.0f) horizIntensity = 0.0f;
        if (horizIntensity > 1.0f) horizIntensity = 1.0f;

        float finalIntensity = targetIntensity > horizIntensity ? targetIntensity : horizIntensity;

        speedIntensity = Lerp(speedIntensity, finalIntensity, 1.0f - expf(-8.0f * dt));

        displaySpeed = Lerp(displaySpeed, rawSpeed, 1.0f - expf(-6.0f * dt));

        Vector2 playerCenter = PlayerCenter(&player);

        Vector2 leadOffset = {
            player.velocity.x * CAMERA_LEAD_FACTOR * 0.25f,
            player.velocity.y * CAMERA_LEAD_FACTOR * 0.12f
        };

        Vector2 desiredTarget = Vector2Add(playerCenter, leadOffset);

        Vector2 diff = Vector2Subtract(desiredTarget, cameraSmooth);
        cameraSmooth = Vector2Add(cameraSmooth, Vector2Scale(diff, 1.0f - expf(-CAMERA_LAG_SPEED * dt)));

        camera.target = cameraSmooth;

        float halfW = SCREEN_W / 2.0f;
        float halfH = SCREEN_H / 2.0f;
        camera.target.x = Clamp(camera.target.x, halfW, worldWidth - halfW);
        camera.target.y = Clamp(camera.target.y, worldTopY + halfH, worldBottomY - halfH);

        float targetZoom = 1.0f - speedIntensity * 0.08f;
        camera.zoom = Lerp(camera.zoom, targetZoom, 1.0f - expf(-6.0f * dt));

        Vector2 camOffset = { SCREEN_W / 2.0f, SCREEN_H / 2.0f };
        if (shakeMagnitude > 0.01f)
        {
            camOffset.x += (float)GetRandomValue(-100, 100) / 100.0f * shakeMagnitude;
            camOffset.y += (float)GetRandomValue(-100, 100) / 100.0f * shakeMagnitude;
            shakeMagnitude *= 0.9f;
        }
        if (speedIntensity > 0.5f)
        {
            float jitter = (speedIntensity - 0.5f) * 4.0f;
            camOffset.x += (float)GetRandomValue(-100, 100) / 100.0f * jitter;
            camOffset.y += (float)GetRandomValue(-100, 100) / 100.0f * jitter * 0.5f;
        }
        camera.offset = camOffset;

        if (speedIntensity > 0.1f)
        {
            int streakCount = (int)(speedIntensity * 4.0f);
            Vector2 sc = { SCREEN_W / 2.0f, SCREEN_H / 2.0f };
            for (int i = 0; i < streakCount; i++)
                SpawnSpeedStreak(player.velocity, sc);
        }
        UpdateSpeedStreaks(dt);

        UpdateTrailGhosts(&player, dt);

        BeginDrawing();
        ClearBackground((Color) { 190, 220, 245, 255 });

        DrawBackground(camera, levelTime);

        BeginMode2D(camera);

        DrawTrailGhosts();

        for (int i = 0; i < solidCount; i++) DrawSolid(&solids[i]);
        for (int i = 0; i < spikeCount; i++) DrawSpike(spikes[i]);
        DrawEnemies(levelTime);
        for (int i = 0; i < coinCount; i++) DrawCoin(&coins[i], levelTime);

        Vector2 pc = PlayerCenter(&player);
        for (int i = 0; i < anchorCount; i++)
        {
            bool inRange = Vector2Distance(pc, anchors[i]) <= GRAPPLE_RANGE;
            DrawAnchor(anchors[i], inRange);
        }

        DrawRectangleRec(goalRect, (Color) { 230, 230, 230, 255 });
        DrawTriangle((Vector2) { goalRect.x + goalRect.width, goalRect.y },
            (Vector2) {
            goalRect.x + goalRect.width, goalRect.y + 22
        },
            (Vector2) {
            goalRect.x + goalRect.width + 34, goalRect.y + 11
        },
            (Color) {
            60, 190, 90, 255
        });

        if (player.grappling)
        {
            DrawRope(PlayerCenter(&player), player.grappleAnchor);
        }
        else
        {
            int aimed = FindBestAnchor(&player);
            if (aimed >= 0)
                DrawDottedLine(PlayerCenter(&player), anchors[aimed], (float)GetTime(), (Color) { 70, 220, 255, 220 });
        }

        DrawParticles();
        if (currentLevel != CUSTOM_LEVEL_INDEX) DrawGhost(currentLevel, levelTime);
        DrawPlayer(&player);

        EndMode2D();

        DrawRadialSpeedLines(player.velocity, speedIntensity);
        DrawSpeedStreaks(camera);
        DrawChromaticAberration(speedIntensity);
        DrawSpeedVignette(speedIntensity);

        DrawRectangle(0, 0, SCREEN_W, 84, Fade(BLACK, 0.35f));
        DrawText("A/D or Arrows: run   SPACE: jump (double-jump in air!)   SHIFT: air dash   F / Click: grapple",
            16, 8, 18, RAYWHITE);
        DrawText("While grappling -> W/S or Up/Down: reel in/out    R: restart    ESC: level select",
            16, 32, 18, RAYWHITE);
        DrawText("Hit a wall hard enough and you'll bounce off it",
            16, 56, 16, (Color) { 220, 220, 220, 255 });

        char hud[128];
        snprintf(hud, sizeof(hud), "Coins: %d/%d      Time: %05.2fs", coinsCollected, coinCount, levelTime);
        int hw = MeasureText(hud, 22);
        DrawText(hud, SCREEN_W - hw - 16, 12, 22, (Color) { 255, 230, 120, 255 });

        DrawMinimap(currentLevel, PlayerCenter(&player), levelTime, (float)GetTime());

        {
            float mph = displaySpeed * MPH_SCALE;

            Color mphColor;
            if (speedIntensity < 0.33f)
            {
                float t = speedIntensity / 0.33f;
                mphColor = (Color){
                    255,
                    (unsigned char)(255 - t * 30),
                    (unsigned char)(255 - t * 200),
                    255
                };
            }
            else if (speedIntensity < 0.66f)
            {
                float t = (speedIntensity - 0.33f) / 0.33f;
                mphColor = (Color){
                    255,
                    (unsigned char)(225 - t * 80),
                    (unsigned char)(55 - t * 40),
                    255
                };
            }
            else
            {
                float t = (speedIntensity - 0.66f) / 0.34f;
                mphColor = (Color){
                    (unsigned char)(255 - t * 30),
                    (unsigned char)(145 - t * 100),
                    (unsigned char)(15),
                    255
                };
            }

            char mphText[32];
            snprintf(mphText, sizeof(mphText), "%3.0f MPH", mph);
            int mphW = MeasureText(mphText, 36);

            DrawRectangle(SCREEN_W / 2 - mphW / 2 - 16, SCREEN_H - 60, mphW + 32, 48,
                Fade(BLACK, 0.45f + speedIntensity * 0.3f));

            float pulse = 1.0f + speedIntensity * 0.08f * sinf(levelTime * 20.0f);
            int fontSize = (int)(36 * pulse);

            DrawText(mphText, SCREEN_W / 2 - mphW / 2, SCREEN_H - 54, fontSize, mphColor);

            float barW = 300.0f;
            float barH = 6.0f;
            float barX = SCREEN_W / 2 - barW / 2;
            float barY = SCREEN_H - 16;

            DrawRectangle((int)barX, (int)barY, (int)barW, (int)barH, Fade(BLACK, 0.5f));
            DrawRectangle((int)barX, (int)barY, (int)(barW * speedIntensity), (int)barH, mphColor);

            for (int i = 1; i < 3; i++)
            {
                int tx = (int)(barX + barW * i / 3.0f);
                DrawRectangle(tx, (int)barY - 2, 1, (int)barH + 4, Fade(WHITE, 0.4f));
            }

            DrawText("SPEED", (int)(barX - 56), (int)(barY - 4), 14, Fade(WHITE, 0.6f));
        }
        if (currentLevel != CUSTOM_LEVEL_INDEX && bestTimes[currentLevel] > 0.0f)
        {
            char best[64];
            snprintf(best, sizeof(best), "Best: %05.2fs", bestTimes[currentLevel]);
            int bw = MeasureText(best, 18);
            DrawText(best, SCREEN_W - bw - 16, 38, 18, (Color) { 200, 220, 255, 255 });
        }

        if (won)
        {
            const char* msg = (currentLevel == CUSTOM_LEVEL_INDEX)
                ? "LEVEL COMPLETE!  R: replay   ESC: back to editor"
                : "LEVEL COMPLETE!  R: replay   ESC: level select";
            int w = MeasureText(msg, 40);
            DrawRectangle(SCREEN_W / 2 - w / 2 - 20, SCREEN_H / 2 - 50, w + 40, 100, Fade(BLACK, 0.6f));
            DrawText(msg, SCREEN_W / 2 - w / 2, SCREEN_H / 2 - 30, 40, (Color) { 250, 220, 80, 255 });
            char sub[96];
            snprintf(sub, sizeof(sub), "Time: %05.2fs   Coins: %d/%d", levelTime, coinsCollected, coinCount);
            int sw = MeasureText(sub, 20);
            DrawText(sub, SCREEN_W / 2 - sw / 2, SCREEN_H / 2 + 18, 20, RAYWHITE);
        }

        DrawFPS(SCREEN_W - 90, SCREEN_H - 24);

        EndDrawing();
    }

    for (int i = 0; i < LEVEL_COUNT; i++) UnloadRenderTexture(levelPreviews[i]);
    for (int i = 0; i < LEVEL_COUNT; i++) free(ghostTracks[i].samples);
    free(recordingTrack.samples);
    ShutdownGameAudio();
    CloseWindow();
    return 0;
}