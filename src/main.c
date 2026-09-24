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
#define JUMP_CUT_MULT     0.45f    // short-hop when space released early
#define COYOTE_TIME       0.10f    // grace period to jump after walking off a ledge
#define JUMP_BUFFER_TIME  0.12f    // grace period for a jump press just before landing

#define PLAYER_W 30.0f
#define PLAYER_H 44.0f

#define GRAPPLE_RANGE     620.0f
#define GRAPPLE_MIN_LEN   70.0f
#define GRAPPLE_REEL_SPD  260.0f
#define GRAPPLE_PULL_ACC  1800.0f  // swing-in assist toward tangential motion

// Wall-bounce: hitting a SOLID_WALL above this speed reflects velocity
// instead of just stopping it dead.
#define WALL_BOUNCE_SPEED_THRESHOLD  480.0f
#define WALL_BOUNCE_RESTITUTION      0.55f   // fraction of speed kept after bounce

#define WORLD_DEATH_Y     1400.0f  // fall past this -> respawn (pit)

#define MAX_SOLIDS  140
#define MAX_SPIKES  60
#define MAX_ANCHORS 48
#define MAX_COINS   100
#define MAX_ENEMIES 60
#define MAX_PARTICLES 300
#define LEVEL_COUNT 4

//------------------------------------------------------------------------------------
// Speed-feel / visual filters
//------------------------------------------------------------------------------------
#define MPH_SCALE            12.0f    // world units/sec -> "MPH" display scale
#define CAMERA_LEAD_FACTOR   0.35f    // how much camera leads in movement direction
#define CAMERA_LAG_SPEED     6.0f     // how quickly camera catches up
#define SPEED_BLUR_MIN       300.0f   // speed at which effects start
#define SPEED_BLUR_MAX       900.0f   // speed at which effects are maxed
#define MOTION_TRAIL_COUNT   6
#define MAX_STREAKS          80

typedef enum { STATE_MENU, STATE_LEVEL_SELECT, STATE_PLAYING } GameState;

typedef enum { SOLID_GROUND, SOLID_FLOATING, SOLID_WALL } SolidType;

typedef struct {
    Rectangle rect;
    SolidType type;
} Solid;

typedef struct {
    Vector2 position;   // top-left
    Vector2 velocity;
    bool onGround;
    bool grappling;
    Vector2 grappleAnchor;
    float ropeLength;
    float facing;       // -1 left, 1 right

    // Juice / feel
    float coyoteTimer;
    float jumpBufferTimer;
    bool  usedDoubleJump;
    Vector2 scale;       // squash & stretch, lerps toward (1,1)

    // Direction anchor->player from the last taut-rope frame, so velocity can
    // be rotated along with the swing instead of leaking speed each frame.
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
    float bob; // animation phase
} Coin;

#define ENEMY_W 30.0f
#define ENEMY_H 26.0f
typedef struct {
    Vector2 pos;   // feet position (bottom-center), sits on the ground it patrols
    float minX, maxX;
    float speed;
    float dir;     // -1 or 1
} Enemy;

// Speed visual effects
typedef struct {
    Vector2 pos;
    float life, maxLife;
    float speed;
    Color color;
    bool alive;
} SpeedStreak;

static SpeedStreak streaks[MAX_STREAKS];

// Motion trail for player
typedef struct {
    Vector2 pos;
    float life;
    Vector2 scale;
    float facing;
    bool grappling;
} TrailGhost;

static TrailGhost trailGhosts[MOTION_TRAIL_COUNT];
static float trailTimer = 0.0f;

// Camera smoothing
static Vector2 cameraSmooth = { 0 };
static Vector2 cameraVel = { 0 };
static float speedIntensity = 0.0f;  // 0..1 normalized speed factor
static float displaySpeed = 0.0f;    // smoothed speed for MPH display

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
static float   worldTopY = -60.0f;   // highest point the camera should show
static float   worldBottomY = 1000.0f; // lowest point the camera should show

static Particle particles[MAX_PARTICLES];

// Screen shake
static float shakeTime = 0.0f;
static float shakeMagnitude = 0.0f;

// Timer
static float levelTime = 0.0f;
static float bestTimes[LEVEL_COUNT] = { -1.0f, -1.0f, -1.0f, -1.0f };
static int currentLevel = 0;
static const char *levelNames[LEVEL_COUNT] = { "Momentum Run", "Spike Alley", "Sky Islands", "Twin Lanes" };

// Sounds (generated procedurally, no external assets needed)
static Sound sndJump, sndDoubleJump, sndLand, sndGrapple, sndWallBounce, sndCoin, sndWin, sndDeath;
static bool audioReady = false;

static void AddSolid(float x, float y, float w, float h, SolidType type)
{
    if (solidCount >= MAX_SOLIDS) return;
    solids[solidCount].rect = (Rectangle){ x, y, w, h };
    solids[solidCount].type = type;
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

// A simple ground patrol enemy: walks between minX and maxX at a fixed y
// (the surface it's standing on), reversing direction at the ends.
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

        DrawEllipse((int)(body.x + body.width * 0.5f), (int)(body.y + body.height * 0.6f),
            body.width * 0.5f, body.height * 0.5f, (Color) { 130, 40, 150, 255 });
        DrawEllipseLines((int)(body.x + body.width * 0.5f), (int)(body.y + body.height * 0.6f),
            body.width * 0.5f, body.height * 0.5f, (Color) { 60, 10, 70, 255 });

        // little spikes on top to read as dangerous
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

        // eyes, looking in the direction of travel
        float eyeDir = enemies[i].dir;
        DrawCircle((int)(body.x + body.width * 0.5f + eyeDir * 5.0f), (int)(body.y + body.height * 0.55f), 3.0f, WHITE);
        DrawCircle((int)(body.x + body.width * 0.5f + eyeDir * 5.0f), (int)(body.y + body.height * 0.55f), 1.3f, BLACK);
    }
}

//------------------------------------------------------------------------------------
// Particles
//------------------------------------------------------------------------------------
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

//------------------------------------------------------------------------------------
// Procedural chiptune sound effects (no external files required)
//------------------------------------------------------------------------------------
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
        float env = 1.0f - (float)i / (float)frameCount; // linear decay envelope
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
    UnloadWave(wave); // frees the malloc'd buffer
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
    CloseAudioDevice();
}

static void Snd(Sound s) { if (audioReady) PlaySound(s); }

//------------------------------------------------------------------------------------
// Level layout
// Ground baseline top = 650. Gaps between ground segments are pits.
// A handful of floating platforms and tall walls require the grapple to cross.
// Coins are sprinkled along risky routes and grapple arcs as an optional
// collect-em-all objective on top of just reaching the goal.
//------------------------------------------------------------------------------------
static void BuildLevel1(void)
{
    // --- Section 1: start, simple gap ---
    AddSolid(0, 650, 520, 100, SOLID_GROUND);
    AddCoin(260, 580);
    // pit: 520 - 660
    AddSolid(660, 650, 380, 100, SOLID_GROUND);
    AddCoin(840, 580);

    // --- Section 2: spikes on the ground ---
    AddSolid(1040, 650, 360, 100, SOLID_GROUND);
    AddSpike(1180, 630, 100, 20);
    AddCoin(1300, 580);

    // --- Section 3: wide pit needing a grapple swing ---
    // pit: 1400 - 1650
    AddAnchor(1520, 380);
    AddCoin(1520, 500); // reward for swinging cleanly through the arc
    AddSolid(1650, 650, 320, 100, SOLID_GROUND);

    // --- Section 4: floating platforms staircase (jumpable) ---
    AddSolid(2050, 540, 150, 30, SOLID_FLOATING);
    AddCoin(2125, 490);
    AddSolid(2260, 420, 150, 30, SOLID_FLOATING);
    AddCoin(2335, 370);
    AddAnchor(2340, 200); // optional grapple assist to the higher platform

    // --- Section 5: drop back down, spiky ground run ---
    AddSolid(2500, 650, 420, 100, SOLID_GROUND);
    AddSpike(2620, 630, 90, 20);
    AddSpike(2780, 630, 90, 20);
    AddCoin(2700, 580);

    // --- Section 6: tall wall, must swing over the top ---
    AddSolid(2960, 380, 40, 370, SOLID_WALL);
    AddAnchor(2980, 220);
    AddCoin(2980, 300);

    // --- Section 7: the Sky Tower ---
    AddSolid(3040, 650, 540, 100, SOLID_GROUND);   // shaft floor
    AddSolid(3300, 250, 40, 300, SOLID_WALL);      // left shaft wall
    AddSolid(3540, 250, 40, 300, SOLID_WALL);      // right shaft wall
    AddAnchor(3440, 560);
    AddAnchor(3440, 430);
    AddAnchor(3440, 300);
    AddAnchor(3440, 170);
    AddCoin(3440, 480);
    AddCoin(3440, 350);
    AddCoin(3440, 220);
    AddSolid(3540, 150, 220, 30, SOLID_FLOATING);  // exit ledge above the right wall
    AddCoin(3650, 100);

    // --- Section 8: floating staircase back down from the tower ---
    AddSolid(3760, 300, 150, 30, SOLID_FLOATING);
    AddSolid(3960, 440, 150, 30, SOLID_FLOATING);
    AddCoin(4035, 390);
    AddSolid(4160, 580, 150, 30, SOLID_FLOATING);
    AddSolid(4360, 650, 400, 100, SOLID_GROUND);
    AddSpike(4480, 630, 90, 20);
    AddSpike(4630, 630, 90, 20);

    // --- Section 9: final big pit + swing, then home stretch ---
    // pit: 4760 - 5060
    AddAnchor(4910, 340);
    AddCoin(4910, 460);
    AddSolid(5060, 650, 800, 100, SOLID_GROUND);
    AddSpike(5280, 630, 90, 20);
    AddSpike(5440, 630, 90, 20);
    AddCoin(5360, 580);

    // Floating bonus platform near the end (optional path)
    AddSolid(5610, 520, 160, 30, SOLID_FLOATING);
    AddCoin(5690, 470);
    AddAnchor(5690, 330);

    goalRect = (Rectangle){ 5760, 580, 40, 70 };

    spawnPoint = (Vector2){ 60, 650 - PLAYER_H };
    worldWidth = 6000.0f;
}

// Level 2: a ground-heavy gauntlet. Spike runs, a wall that needs a double
// jump (or the anchor above it), and pits sized for a double jump or swing.
static void BuildLevel2(void)
{
    AddSolid(0, 650, 600, 100, SOLID_GROUND);
    AddCoin(300, 580);

    // Spike run
    AddSolid(600, 650, 900, 100, SOLID_GROUND);
    AddSpike(780, 630, 90, 20);
    AddSpike(960, 630, 90, 20);
    AddSpike(1140, 630, 90, 20);
    AddSpike(1320, 630, 90, 20);
    AddCoin(870, 540);
    AddCoin(1050, 540);
    AddCoin(1230, 540);

    // Pit 1500 - 1760: swing across
    AddAnchor(1630, 380);
    AddCoin(1630, 500);
    AddSolid(1760, 650, 760, 100, SOLID_GROUND);
    AddSpike(1900, 630, 90, 20);
    AddSpike(2080, 630, 90, 20);
    AddCoin(1985, 560);

    // Wall taller than a single jump: double jump, or swing off the anchor
    AddSolid(2300, 480, 40, 170, SOLID_WALL);
    AddAnchor(2320, 300);
    AddCoin(2320, 400);

    // Wide pit 2520 - 2860: double jump or chain the two anchors
    AddAnchor(2620, 400);
    AddAnchor(2780, 380);
    AddCoin(2700, 470);
    AddSolid(2860, 650, 840, 100, SOLID_GROUND);
    AddSpike(3000, 630, 90, 20);
    AddSpike(3180, 630, 90, 20);
    AddSpike(3360, 630, 90, 20);
    AddCoin(3090, 540);
    AddCoin(3270, 540);

    // Last pit 3700 - 3950
    AddAnchor(3830, 400);
    AddCoin(3830, 520);
    AddSolid(3950, 650, 450, 100, SOLID_GROUND);
    AddSpike(4080, 630, 90, 20);

    goalRect = (Rectangle){ 4300, 580, 40, 70 };
    spawnPoint = (Vector2){ 60, 650 - PLAYER_H };
    worldWidth = 4500.0f;
}

// Level 3: floating islands over a bottomless drop. Every gap is crossable
// with a double jump; the anchors are shortcuts and coin routes.
static void BuildLevel3(void)
{
    AddSolid(0, 650, 420, 100, SOLID_GROUND);
    AddCoin(200, 580);

    // Rising staircase of islands
    AddSolid(600, 600, 180, 30, SOLID_FLOATING);
    AddCoin(690, 550);
    AddSolid(960, 540, 180, 30, SOLID_FLOATING);
    AddCoin(1050, 490);
    AddSolid(1320, 470, 160, 30, SOLID_FLOATING);
    AddAnchor(1150, 250);
    AddCoin(1150, 330);

    // 300 gap
    AddAnchor(1630, 250);
    AddSolid(1780, 470, 160, 30, SOLID_FLOATING);
    AddCoin(1630, 350);

    AddSolid(2150, 400, 160, 30, SOLID_FLOATING);
    AddCoin(2230, 350);
    AddSolid(2500, 330, 160, 30, SOLID_FLOATING);
    AddSolid(2850, 260, 160, 30, SOLID_FLOATING);
    AddCoin(2930, 210);

    // Platform with a wall to clear (double jump, or swing over it)
    AddSolid(3150, 260, 260, 30, SOLID_FLOATING);
    AddSolid(3300, 100, 40, 160, SOLID_WALL);
    AddAnchor(3320, 20);
    AddCoin(3320, 60);

    AddSolid(3600, 300, 160, 30, SOLID_FLOATING);
    AddCoin(3680, 250);

    // Long gap 3760 - 4200: chain the anchors (or a very good double jump)
    AddAnchor(3900, 140);
    AddAnchor(4080, 130);
    AddCoin(3990, 260);
    AddSolid(4200, 400, 180, 30, SOLID_FLOATING);

    AddSolid(4560, 470, 160, 30, SOLID_FLOATING);
    AddCoin(4640, 420);

    // Home stretch
    AddSolid(4900, 650, 700, 100, SOLID_GROUND);
    AddSpike(5100, 630, 90, 20);
    AddSpike(5280, 630, 90, 20);
    AddCoin(5190, 560);

    goalRect = (Rectangle){ 5500, 580, 40, 70 };
    spawnPoint = (Vector2){ 60, 650 - PLAYER_H };
    worldWidth = 5700.0f;
}

// Level 4: a long twin-lane level (ground route + grapple route) built from
// reusable section patterns; contributed on the sean branch.
#define SECTION_WIDTH      760.0f
#define SECTION_WIDTH_ALT  680.0f   // second-half sections are a different width,
                                    // so the rhythm of gaps/anchors never lines up

// =====================================================================================
// LOWER LANE - ACT I (ground route, first half)
// =====================================================================================
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

// =====================================================================================
// LOWER LANE - ACT II (ground route, second half)
//
// These are deliberately NOT re-skins of the Act I patterns. The whole lane
// sits 40px lower (baseline y=690) so the second half has its own visual and
// spatial identity, and every pattern uses a different shape language:
//   - comb/spike fields instead of single spikes
//   - overhangs and crawl spaces instead of open plateaus
//   - moving-enemy corridors instead of static gauntlets
//   - multi-tier steps instead of a symmetric staircase
// =====================================================================================

// A low, wide "comb" of many small spikes - you have to commit to a full
// running jump and clear the whole field in one go, rather than hopping over
// isolated blades.
static void Lower2_SpikeComb(float x0)
{
    AddSolid(x0, 690, SECTION_WIDTH_ALT, 100, SOLID_GROUND);
    // Five evenly spaced spikes; the last one is right at the edge of a
    // full-speed jump arc, so you can't take it lazily.
    for (int i = 0; i < 5; i++)
        AddSpike(x0 + 180 + i * 80, 670, 40, 20);
    AddCoin(x0 + SECTION_WIDTH_ALT * 0.5f, 600);
}

// A raised ridge with a patrol enemy on top AND a spike on the far side of
// the landing zone - you have to decide whether to fight the enemy or take
// the harder long jump over both hazards.
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

// A crawl-space corridor: solid rock overhead with just enough clearance to
// run through, but a low spike strip on the floor means you have to jump
// *just* right without hitting your head. Different from the Act I tunnel
// (which was enemy-driven and had no floor hazard).
static void Lower2_CrawlSpikes(float x0)
{
    float entryW = 100.0f;
    float crawlW = 440.0f;
    float exitW = SECTION_WIDTH_ALT - entryW - crawlW;

    float clearance = 50.0f;   // slightly taller than the Act I tunnel
    float ceilBottomY = 690.0f - PLAYER_H - clearance;
    float ceilThickness = 80.0f;

    AddSolid(x0, 690, entryW, 100, SOLID_GROUND);
    AddSolid(x0 + entryW, 690, crawlW, 100, SOLID_GROUND);
    AddSolid(x0 + entryW, ceilBottomY - ceilThickness, crawlW, ceilThickness, SOLID_WALL);
    // two low spike strips, forcing a short hop between them
    AddSpike(x0 + entryW + 90, 670, 70, 20);
    AddSpike(x0 + entryW + 280, 670, 70, 20);
    AddSolid(x0 + entryW + crawlW, 690, exitW, 100, SOLID_GROUND);
    AddCoin(x0 + entryW + crawlW - 60, 640);
}

// A drop into a narrow chasm with a spike floor, a single floating step in
// the middle, and a climb out on the far side. Much tighter than the Act I
// sunken trench.
static void Lower2_ChasmHop(float x0)
{
    float entryW = 160.0f;
    float chasmW = 360.0f;
    float exitW = SECTION_WIDTH_ALT - entryW - chasmW;

    AddSolid(x0, 690, entryW, 100, SOLID_GROUND);
    // chasm floor is low, with spikes along it
    AddSolid(x0 + entryW, 690 + 160, chasmW, 100, SOLID_GROUND);
    AddSpike(x0 + entryW + 100, 670 + 160, 160, 20);
    // one small floating stepping stone in the middle of the chasm
    AddSolid(x0 + entryW + chasmW * 0.5f - 40, 690 + 40, 80, 24, SOLID_FLOATING);
    AddCoin(x0 + entryW + chasmW * 0.5f, 690 + 10);
    AddSolid(x0 + entryW + chasmW, 690, exitW, 100, SOLID_GROUND);
}

// A three-tier descent: down a step, along a shelf, down another step, along
// a lower shelf, then back up. The Act I staircase went *up then down*; this
// one only ever descends, which reads very differently in motion.
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

// A corridor with two enemies patrolling in opposite phases - you have to
// thread between them rather than simply jump one. Distinct from the Act I
// EnemyGauntlet, which had both enemies on flat open ground.
static void Lower2_EnemyPincer(float x0)
{
    float entryW = 140.0f;
    float corridorW = 400.0f;
    float exitW = SECTION_WIDTH_ALT - entryW - corridorW;

    AddSolid(x0, 690, entryW, 100, SOLID_GROUND);
    AddSolid(x0 + entryW, 690, corridorW, 100, SOLID_GROUND);
    // Two enemies whose patrol ranges overlap in the middle - they cross.
    AddEnemy(x0 + entryW + 110, 690, x0 + entryW + 30.0f,
        x0 + entryW + corridorW * 0.65f, 130.0f);
    AddEnemy(x0 + entryW + corridorW - 110, 690,
        x0 + entryW + corridorW * 0.35f,
        x0 + entryW + corridorW - 30.0f, 130.0f);
    AddSolid(x0 + entryW + corridorW, 690, exitW, 100, SOLID_GROUND);
    AddCoin(x0 + entryW + corridorW * 0.5f, 620);
}

// A wide pit you cross via two floating stepping stones - requires either
// two well-timed jumps or a grapple off a nearby anchor. Different from the
// Act I RunningJumpPit (single gap, one jump).
static void Lower2_SteppingPit(float x0)
{
    float g1 = 220.0f;
    float pitW = 380.0f;
    float g2 = SECTION_WIDTH_ALT - g1 - pitW;

    AddSolid(x0, 690, g1, 100, SOLID_GROUND);
    AddSolid(x0 + g1 + pitW, 690, g2, 100, SOLID_GROUND);
    // two stepping stones over the pit
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

// =====================================================================================
// UPPER LANE - ACT I (grapple route, first half)
// =====================================================================================
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

// =====================================================================================
// UPPER LANE - ACT II (grapple route, second half)
//
// Different anchor geometry and different rest-platform shapes. Where Act I
// alternated single anchors with small floating rests, Act II uses:
//   - paired anchors close together (double-swing chains)
//   - long vertical chains that climb rather than cross
//   - rest platforms that are *moving-feeling* (thin and offset) rather than
//     big flat rectangles
// =====================================================================================

// Two anchors close together, forcing a fast release-and-refire rather than
// one big pendulum.
static void Upper2_DoubleSwing(float x0)
{
    AddAnchor(x0 + 200, 320);
    AddAnchor(x0 + 460, 240);
    AddCoin(x0 + 330, 380);
}

// A vertical ladder of three anchors - you reel in on each to climb, more
// like an ascent than a swing.
static void Upper2_VerticalLadder(float x0)
{
    AddAnchor(x0 + 360, 380);
    AddAnchor(x0 + 360, 270);
    AddAnchor(x0 + 360, 160);
    AddCoin(x0 + 360, 90);
}

// A swing out over a long gap onto a thin offset ledge (rather than a wide
// flat rest platform), with the anchor placed so you naturally land moving.
static void Upper2_OffsetLedge(float x0)
{
    AddAnchor(x0 + 220, 290);
    AddSolid(x0 + 480, 360, 110, 22, SOLID_FLOATING);
    AddCoin(x0 + 520, 310);
}

// A high anchor that you swing *under*, with the exit coin placed low so
// you have to let go early and fall to grab it.
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

// ---- Act-specific twin-lane builders (different section width per act) ----
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

// Returns the total width consumed by an Act II stretch, since the caller
// needs it to place the next landmark.
static float Act2StretchWidth(int numSections)
{
    return numSections * SECTION_WIDTH_ALT;
}

static void BuildLevel4(void)
{

    // --- Shared intro: everyone starts on the same simple ground run ---
    AddSolid(0, 650, 520, 100, SOLID_GROUND);
    AddCoin(260, 580);
    // pit: 520 - 660
    AddSolid(660, 650, 240, 100, SOLID_GROUND);
    AddCoin(780, 580);

    // --- The Fork: a stepping stone + anchor mark the entrance to the upper route ---
    float forkX = 900.0f;
    AddSolid(forkX - 60, 520, 120, 30, SOLID_FLOATING); // hop up here to start grappling
    AddAnchor(forkX + 60, 340);
    AddCoin(forkX - 20, 470);

    // --- Twin-lane stretch #1 (Act I patterns) ---
    const int N1 = 10;
    BuildTwinLaneStretchAct1(forkX, N1, 0, 0);
    float towerX = forkX + N1 * SECTION_WIDTH;

    // --- The Sky Tower: both lanes are forced to converge here and grapple up ---
    AddSolid(towerX, 650, 540, 100, SOLID_GROUND);   // shaft floor
    AddSolid(towerX + 260, 250, 40, 300, SOLID_WALL); // left shaft wall
    AddSolid(towerX + 500, 250, 40, 300, SOLID_WALL); // right shaft wall
    AddAnchor(towerX + 380, 560);
    AddAnchor(towerX + 380, 430);
    AddAnchor(towerX + 380, 300);
    AddAnchor(towerX + 380, 170);
    AddCoin(towerX + 380, 480);
    AddCoin(towerX + 380, 350);
    AddCoin(towerX + 380, 220);
    AddSolid(towerX + 500, 150, 220, 30, SOLID_FLOATING); // exit ledge above the right wall
    AddCoin(towerX + 610, 100);

    // --- Descend back down from the tower into a floating staircase, then split lanes again ---
    float descentX = towerX + SECTION_WIDTH;
    AddSolid(descentX, 300, 150, 30, SOLID_FLOATING);
    AddSolid(descentX + 220, 440, 150, 30, SOLID_FLOATING);
    AddCoin(descentX + 295, 390);
    AddSolid(descentX + 420, 580, 150, 30, SOLID_FLOATING);
    AddSolid(descentX + 620, 650, 300, 100, SOLID_GROUND); // touches back down to ground level
    AddSpike(descentX + 700, 630, 90, 20);

    // --- The second fork: a visually distinct entrance. Instead of a hop-up
    //     stepping stone, this one is a *drop-in*: a raised lip you walk off,
    //     with the first anchor already in range on the way down. ---
    float fork2X = descentX + 1000.0f;
    // raised lip the player runs off
    AddSolid(fork2X - 80, 650 - 80, 160, 180, SOLID_GROUND);
    // a coin under the lip, encouraging the drop-in
    AddCoin(fork2X + 40, 580);
    // first anchor of Act II, placed low so you catch it on the way down
    AddAnchor(fork2X + 160, 420);

    // --- Twin-lane stretch #2 (Act II patterns, narrower sections, lower baseline) ---
    const int N2 = 9;
    float act2Start = fork2X + 200.0f;   // slight gap after the drop-in lip
    BuildTwinLaneStretchAct2(act2Start, N2, 0, 0);
    float finalX = act2Start + Act2StretchWidth(N2);

    // --- Final convergence stretch, then the goal ---
    // Blend the two baselines: a short ramp down from the Act II baseline
    // (690) to the original 650, so the finish reads as "coming back home".
    AddSolid(finalX, 690, 260, 100, SOLID_GROUND);
    AddSolid(finalX + 260, 650, 540, 100, SOLID_GROUND);
    AddSpike(finalX + 380, 630, 90, 20);
    AddSpike(finalX + 580, 630, 90, 20);
    AddCoin(finalX + 720, 580);

    AddSolid(finalX + 900, 520, 160, 30, SOLID_FLOATING); // optional bonus detour near the end
    AddCoin(finalX + 980, 470);
    AddAnchor(finalX + 980, 330);

    goalRect = (Rectangle){ finalX + 1200, 580, 40, 70 };
    worldWidth = finalX + 1200 + 300;

    spawnPoint = (Vector2){ 60, 650 - PLAYER_H };
}

static void BuildLevel(int index)
{
    solidCount = spikeCount = anchorCount = coinCount = enemyCount = 0;
    switch (index)
    {
        case 1:  BuildLevel2(); break;
        case 2:  BuildLevel3(); break;
        case 3:  BuildLevel4(); break;
        default: BuildLevel1(); break;
    }
}

//------------------------------------------------------------------------------------
// Collision helpers
//------------------------------------------------------------------------------------
static Rectangle PlayerRect(Vector2 pos)
{
    return (Rectangle) { pos.x, pos.y, PLAYER_W, PLAYER_H };
}

static Vector2 PlayerCenter(Player* p)
{
    return (Vector2) { p->position.x + PLAYER_W * 0.5f, p->position.y + PLAYER_H * 0.5f };
}

static void MoveAndCollide(Player* p, float dt)
{
    bool wasOnGround = p->onGround;

    // --- Horizontal ---
    p->position.x += p->velocity.x * dt;
    Rectangle pr = PlayerRect(p->position);
    for (int i = 0; i < solidCount; i++)
    {
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

    // --- Vertical ---
    p->onGround = false;
    p->position.y += p->velocity.y * dt;
    pr = PlayerRect(p->position);
    for (int i = 0; i < solidCount; i++)
    {
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

    // Landing feedback: squash + dust burst, only on the frame we touch down
    if (p->onGround && !wasOnGround)
    {
        p->scale = (Vector2){ 1.35f, 0.65f };
        Snd(sndLand);
        Shake(3.0f, 0.08f);
        Vector2 feet = { p->position.x + PLAYER_W * 0.5f, p->position.y + PLAYER_H };
        SpawnBurst(feet, 8, 140.0f, 0.4f, 3.5f, (Color) { 210, 200, 170, 255 });
        p->usedDoubleJump = false;
        p->coyoteTimer = 0.0f;
    }
}

// The grapple constraint repositions the player directly, which can leave
// the body edge-on into a wall the rope circle happened to pass through.
// This pushes the player back out along the shallowest overlap axis.
static void ResolveSolidOverlap(Player* p)
{
    Rectangle pr = PlayerRect(p->position);
    for (int i = 0; i < solidCount; i++)
    {
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
    p->scale = (Vector2){ 1.0f, 1.0f };
}

//------------------------------------------------------------------------------------
// Ghost replay: the current run is sampled at a fixed rate; when a run sets a
// personal best, that recording becomes the level's ghost and is played back
// (in sync with the run timer) on later attempts.
//------------------------------------------------------------------------------------
#define GHOST_HZ            60.0f
#define GHOST_MAX_SAMPLES   54000   // 15 minutes; longer runs simply don't record a ghost

typedef struct {
    Vector2 pos;      // player top-left
    Vector2 anchor;   // grapple anchor (valid when grappling)
    float facing;
    bool grappling;
} GhostSample;

typedef struct {
    GhostSample* samples;
    int count, cap;
} GhostTrack;

static GhostTrack recordingTrack;              // the run in progress
static bool recordingOverflow = false;         // run too long to keep
static GhostTrack ghostTracks[LEVEL_COUNT];    // best run per level

static void RecordingReset(void)
{
    recordingTrack.count = 0;
    recordingOverflow = false;
}

// Pushes one sample per elapsed 1/GHOST_HZ of run time, so playback doesn't
// depend on the frame rate the run was recorded at.
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

static void DrawGhost(int level, float t)
{
    GhostTrack* g = &ghostTracks[level];
    if (g->count < 2) return;

    float f = t * GHOST_HZ;
    int i = (int)f;
    GhostSample s;
    if (i >= g->count - 1)
    {
        s = g->samples[g->count - 1];
    }
    else
    {
        GhostSample a = g->samples[i], b = g->samples[i + 1];
        float k = f - (float)i;
        s = a;
        s.pos = Vector2Lerp(a.pos, b.pos, k);
        s.anchor = Vector2Lerp(a.anchor, b.anchor, k);
    }

    Color body = (Color){ 130, 210, 255, 110 };
    Color edge = (Color){ 40, 110, 170, 150 };
    if (s.grappling)
        DrawLineEx((Vector2){ s.pos.x + PLAYER_W * 0.5f, s.pos.y + PLAYER_H * 0.5f }, s.anchor, 1.5f, (Color){ 130, 210, 255, 90 });
    Rectangle r = { s.pos.x, s.pos.y, PLAYER_W, PLAYER_H };
    DrawRectangleRec(r, body);
    DrawRectangleLinesEx(r, 2, edge);
    DrawCircle((int)(r.x + PLAYER_W * 0.5f + s.facing * 8.0f), (int)(r.y + 14), 3, (Color){ 255, 255, 255, 170 });
    const char* tag = "PB";
    DrawText(tag, (int)(r.x + PLAYER_W * 0.5f - MeasureText(tag, 12) * 0.5f), (int)r.y - 16, 12, (Color){ 130, 210, 255, 220 });
}

// Level-select previews: each level is drawn once into a small texture at
// startup (world scaled to fit, key features enlarged so they stay readable).
#define PREVIEW_W 456
#define PREVIEW_H 128
static RenderTexture2D levelPreviews[LEVEL_COUNT];

static void BakeLevelPreview(int index)
{
    BuildLevel(index);

    float minY = goalRect.y;
    for (int i = 0; i < solidCount; i++) minY = fminf(minY, solids[i].rect.y);
    for (int i = 0; i < anchorCount; i++) minY = fminf(minY, anchors[i].y);
    for (int i = 0; i < coinCount; i++) minY = fminf(minY, coins[i].pos.y);
    minY -= 40.0f;
    float maxY = 720.0f;

    // Scale x to fit the whole level; y fits the height but is capped at 3x the
    // x scale, so very long levels get a stretched (readable) profile instead
    // of a hairline. Level is vertically centered in the texture.
    float sx = (float)PREVIEW_W / worldWidth;
    float sy = fminf((float)PREVIEW_H / (maxY - minY), sx * 3.0f);
    float offY = ((float)PREVIEW_H - (maxY - minY) * sy) * 0.5f;
#define PV_X(wx) ((wx) * sx)
#define PV_Y(wy) (((wy) - minY) * sy + offY)

    levelPreviews[index] = LoadRenderTexture(PREVIEW_W, PREVIEW_H);
    SetTextureFilter(levelPreviews[index].texture, TEXTURE_FILTER_BILINEAR);

    BeginTextureMode(levelPreviews[index]);
    DrawRectangleGradientV(0, 0, PREVIEW_W, PREVIEW_H, (Color){ 150, 200, 240, 255 }, (Color){ 225, 240, 250, 255 });

    for (int i = 0; i < solidCount; i++)
    {
        Color c;
        switch (solids[i].type)
        {
            case SOLID_FLOATING: c = (Color){ 120, 160, 220, 255 }; break;
            case SOLID_WALL:     c = (Color){ 90, 90, 100, 255 };  break;
            default:             c = (Color){ 80, 130, 80, 255 };  break;
        }
        Rectangle r = solids[i].rect;
        // Keep thin platforms visible: at least 2px in each direction.
        Rectangle pr = { PV_X(r.x), PV_Y(r.y), fmaxf(r.width * sx, 2.0f), fmaxf(r.height * sy, 2.0f) };
        DrawRectangleRec(pr, c);
    }
    for (int i = 0; i < spikeCount; i++)
    {
        Rectangle r = spikes[i];
        float w = fmaxf(r.width * sx, 3.0f);
        Vector2 base = { PV_X(r.x), PV_Y(r.y + r.height) };
        DrawTriangle((Vector2){ base.x, base.y }, (Vector2){ base.x + w * 0.5f, base.y - 4.0f },
                     (Vector2){ base.x + w, base.y }, (Color){ 190, 40, 40, 255 });
    }
    for (int i = 0; i < enemyCount; i++)
        DrawCircleV((Vector2){ PV_X(enemies[i].pos.x), PV_Y(enemies[i].pos.y) - 2.0f }, 2.4f, (Color){ 130, 40, 150, 255 });
    for (int i = 0; i < coinCount; i++)
        DrawCircleV((Vector2){ PV_X(coins[i].pos.x), PV_Y(coins[i].pos.y) }, 2.0f, (Color){ 255, 215, 60, 255 });
    for (int i = 0; i < anchorCount; i++)
        DrawCircleV((Vector2){ PV_X(anchors[i].x), PV_Y(anchors[i].y) }, 2.6f, (Color){ 90, 110, 170, 255 });
    DrawRectangle((int)PV_X(goalRect.x) - 2, (int)PV_Y(goalRect.y) - 4, 6, (int)fmaxf(goalRect.height * sy, 8.0f) + 4, (Color){ 60, 190, 90, 255 });
    DrawCircleV((Vector2){ PV_X(spawnPoint.x), PV_Y(spawnPoint.y) }, 3.4f, (Color){ 200, 60, 60, 255 });

    EndTextureMode();
#undef PV_X
#undef PV_Y
}

// Builds the given level and resets all per-run state (also used for restart).
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

//------------------------------------------------------------------------------------
// Grapple
//------------------------------------------------------------------------------------

// Whether the segment from origin, going maxDist along the unit vector dir,
// intersects rectangle r. Built from raylib's own point/line collision
// checks rather than a hand-rolled slab test.
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

// The rope is only allowed to refuse shortening when a wall is genuinely
// in the way of the player right now: it crosses the line from the anchor
// to the player, AND the player's body is actually touching that same
// wall. A slightly inflated player rect catches the case where they're
// flush against it (MoveAndCollide stops them exactly at the surface,
// not overlapping it).
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

// The anchor a grapple shot would attach to right now (nearest within range),
// or -1. Shared by firing and the aim indicator so they always agree.
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
        p->usedDoubleJump = false; // grappling refreshes the double jump for extra chaining fun
        Snd(sndGrapple);
        SpawnBurst(anchors[best], 10, 160.0f, 0.4f, 3.0f, (Color) { 70, 220, 255, 255 });
    }
}

static void UpdateGrapple(Player* p, float dt)
{
    if (!p->grappling) return;

    // Reel in / out. Shortening is refused while a wall sits between the
    // anchor and the player and the player is touching it; lengthening is
    // always allowed.
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

        // The player moved in a straight line this frame, so the rope direction
        // turned by some angle. Turn the velocity by the same angle first;
        // otherwise removing the "outward" part below deletes real speed every
        // frame (worst on short ropes and low frame rates).
        if (p->ropeDirValid)
            p->velocity = Vector2Rotate(p->velocity, Vector2Angle(p->ropeDir, dir));

        // Snap back onto the rope circle
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
        // Slack rope: free flight, so the last direction no longer applies.
        p->ropeDirValid = false;
    }

    // A steady trail of little sparks along the rope makes the swing read
    // as "powered" rather than just a static line.
    if (GetRandomValue(0, 100) < 40)
    {
        float t = (float)GetRandomValue(20, 80) / 100.0f;
        Vector2 pt = Vector2Lerp(center, p->grappleAnchor, t);
        SpawnParticle(pt, (Vector2) { 0, -20 }, 0.25f, 2.0f, (Color) { 255, 240, 180, 200 });
    }
}

//------------------------------------------------------------------------------------
// Drawing
//------------------------------------------------------------------------------------
//------------------------------------------------------------------------------------
// Speed effects
//------------------------------------------------------------------------------------
static void UpdateSpeedStreaks(float dt)
{
    for (int i = 0; i < MAX_STREAKS; i++)
    {
        if (!streaks[i].alive) continue;
        streaks[i].life -= dt;
        if (streaks[i].life <= 0.0f) { streaks[i].alive = false; continue; }
        // Streaks move opposite to player velocity, in screen space
        streaks[i].pos.x -= streaks[i].speed * 0.02f * (streaks[i].color.r > 200 ? 1.0f : -1.0f);
    }
}

static void SpawnSpeedStreak(Vector2 playerVel, Vector2 screenCenter)
{
    if (speedIntensity < 0.15f) return;

    for (int i = 0; i < MAX_STREAKS; i++)
    {
        if (streaks[i].alive) continue;

        // Spawn at edges of screen, biased perpendicular to motion
        float angle = atan2f(-playerVel.y, -playerVel.x);
        float spread = (GetRandomValue(0, 100) / 100.0f - 0.5f) * 2.0f;
        float perpAngle = angle + PI * 0.5f + spread * 0.8f;

        // Distance from center scaled by how fast we're going
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
    Vector2 screenCenter = { SCREEN_W / 2.0f, SCREEN_H / 2.0f };
    Vector2 playerVel = { 0, 0 }; // will be set externally

    for (int i = 0; i < MAX_STREAKS; i++)
    {
        if (!streaks[i].alive) continue;
        float t = streaks[i].life / streaks[i].maxLife;

        // Draw as horizontal-ish streak
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

// Radial speed lines emanating from screen edges when going fast
static void DrawRadialSpeedLines(Vector2 playerVel, float intensity)
{
    if (intensity < 0.05f) return;

    Vector2 center = { SCREEN_W / 2.0f, SCREEN_H / 2.0f };

    // Direction of movement determines which side lines come from
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

// Vignette that intensifies with speed
static void DrawSpeedVignette(float intensity)
{
    if (intensity < 0.1f) return;

    unsigned char alpha = (unsigned char)(intensity * 120);
    Color c = (Color){ 20, 10, 40, alpha };

    // Draw gradient rectangles at edges
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

// Chromatic aberration effect - draws red/blue offset copies at edges
static void DrawChromaticAberration(float intensity)
{
    if (intensity < 0.2f) return;

    float offset = intensity * 6.0f;
    unsigned char alpha = (unsigned char)(intensity * 70);

    // Simple edge tinting to fake chromatic aberration
    Color redTint = { 255, 0, 0, alpha };
    Color blueTint = { 0, 80, 255, alpha };

    // Red on left edge, blue on right (or based on movement)
    DrawRectangle(0, 0, (int)(offset * 3), SCREEN_H, redTint);
    DrawRectangle(SCREEN_W - (int)(offset * 3), 0, (int)(offset * 3), SCREEN_H, blueTint);
}

// Motion trail ghosts
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

            // Shift ghosts down
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

static void DrawParallaxBackground(Camera2D camera)
{
    // Sky gradient
    DrawRectangleGradientV(0, 0, SCREEN_W, SCREEN_H, (Color) { 150, 200, 240, 255 }, (Color) { 225, 240, 250, 255 });

    // Far hills (slow parallax)
    float p1 = camera.target.x * 0.15f;
    Color hillFar = (Color){ 170, 200, 175, 255 };
    for (int i = -1; i < 6; i++)
    {
        float bx = i * 500.0f - fmodf(p1, 500.0f);
        DrawCircle((int)bx + 150, SCREEN_H - 40, 220, hillFar);
    }

    // Near hills (faster parallax)
    float p2 = camera.target.x * 0.35f;
    Color hillNear = (Color){ 140, 185, 150, 255 };
    for (int i = -1; i < 6; i++)
    {
        float bx = i * 380.0f - fmodf(p2, 380.0f);
        DrawCircle((int)bx + 120, SCREEN_H + 10, 170, hillNear);
    }

    // Drifting clouds
    float p3 = camera.target.x * 0.08f;
    for (int i = -1; i < 5; i++)
    {
        float cx = i * 620.0f - fmodf(p3, 620.0f);
        float cy = 90.0f + 40.0f * sinf((float)i * 1.7f);
        Color cloud = (Color){ 255, 255, 255, 200 };
        DrawCircle((int)cx, (int)cy, 26, cloud);
        DrawCircle((int)cx + 28, (int)cy + 8, 20, cloud);
        DrawCircle((int)cx - 26, (int)cy + 10, 18, cloud);
    }
}

// Input check for a rectangle button -- safe to call outside BeginDrawing.
static bool IsButtonClicked(Rectangle rect)
{
    return CheckCollisionPointRec(GetMousePosition(), rect) && IsMouseButtonPressed(MOUSE_BUTTON_LEFT);
}

// Draws a button, highlighted and slightly enlarged while the mouse hovers it.
static void DrawButton(Rectangle rect, const char *label, int fontSize)
{
    bool hovered = CheckCollisionPointRec(GetMousePosition(), rect);
    Rectangle r = rect;
    if (hovered)
    {
        r.x -= 5; r.y -= 5; r.width += 10; r.height += 10;
    }

    Color border = (Color){ 40, 40, 60, 255 };
    Color bg = hovered ? (Color){ 250, 220, 120, 255 } : (Color){ 235, 235, 245, 255 };

    Rectangle shadow = { r.x + 3, r.y + 5, r.width, r.height };
    DrawRectangleRounded(shadow, 0.3f, 8, Fade(BLACK, 0.25f));
    DrawRectangleRounded(r, 0.3f, 8, bg);
    DrawRectangleRoundedLinesEx(r, 0.3f, 8, 2.0f, border);

    int tw = MeasureText(label, fontSize);
    DrawText(label, (int)(r.x + r.width * 0.5f - tw * 0.5f),
             (int)(r.y + r.height * 0.5f - fontSize * 0.5f), fontSize, border);
}

// A level-select card: big number, name, and best time. Grows when hovered.
static void DrawLevelCard(Rectangle rect, int number, const char* name, float best, Texture2D preview)
{
    bool hovered = CheckCollisionPointRec(GetMousePosition(), rect);
    Rectangle r = rect;
    if (hovered)
    {
        r.x -= 6; r.y -= 6; r.width += 12; r.height += 12;
    }

    Color border = (Color){ 40, 40, 60, 255 };
    Color bg = hovered ? (Color){ 250, 220, 120, 255 } : (Color){ 235, 235, 245, 255 };

    DrawRectangleRounded((Rectangle){ r.x + 4, r.y + 6, r.width, r.height }, 0.12f, 8, Fade(BLACK, 0.25f));
    DrawRectangleRounded(r, 0.12f, 8, bg);
    DrawRectangleRoundedLinesEx(r, 0.12f, 8, 2.0f, border);

    char num[8];
    snprintf(num, sizeof(num), "%d", number);
    int nw = MeasureText(num, 56);
    DrawText(num, (int)(r.x + r.width * 0.5f - nw * 0.5f), (int)(r.y + 12), 56, border);

    int lw = MeasureText(name, 24);
    DrawText(name, (int)(r.x + r.width * 0.5f - lw * 0.5f), (int)(r.y + 74), 24, border);

    Rectangle pv = { r.x + 16, r.y + 108, r.width - 32, (r.width - 32) * PREVIEW_H / PREVIEW_W };
    DrawTexturePro(preview, (Rectangle){ 0, 0, (float)preview.width, -(float)preview.height }, pv,
                   (Vector2){ 0, 0 }, 0.0f, WHITE);
    DrawRectangleLinesEx(pv, 2.0f, border);

    char bestText[32];
    if (best > 0.0f) snprintf(bestText, sizeof(bestText), "Best: %05.2fs", best);
    else snprintf(bestText, sizeof(bestText), "Best: --");
    int bw = MeasureText(bestText, 18);
    DrawText(bestText, (int)(r.x + r.width * 0.5f - bw * 0.5f), (int)(r.y + r.height - 32), 18, (Color){ 90, 90, 110, 255 });
}

// Deterministic twinkling starfield overlay (no particle/physics state needed).
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

static void DrawSolid(Solid *s)
{
    Color c;
    switch (s->type)
    {
    case SOLID_FLOATING: c = (Color){ 120, 160, 220, 255 }; break;
    case SOLID_WALL:     c = (Color){ 90, 90, 100, 255 }; break;
    default:             c = (Color){ 80, 130, 80, 255 }; break;
    }
    DrawRectangleRec(s->rect, c);
    DrawRectangleLinesEx(s->rect, 2, (Color) { 30, 30, 30, 255 });
    if (s->type == SOLID_GROUND)
    {
        DrawRectangle((int)s->rect.x, (int)s->rect.y, (int)s->rect.width, 8, (Color) { 110, 190, 90, 255 });
    }
}

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
        DrawTriangle(p1, p2, p3, (Color) { 190, 40, 40, 255 });
        DrawTriangleLines(p1, p2, p3, (Color) { 90, 10, 10, 255 });
    }
}

// Dots marching from a toward b, so the aim line reads as "pulling toward" the target.
static void DrawDottedLine(Vector2 a, Vector2 b, float time, Color color)
{
    const float spacing = 16.0f;
    float dist = Vector2Distance(a, b);
    if (dist < 1.0f) return;
    Vector2 dir = Vector2Scale(Vector2Subtract(b, a), 1.0f / dist);
    for (float d = fmodf(time * 40.0f, spacing); d < dist; d += spacing)
        DrawCircleV(Vector2Add(a, Vector2Scale(dir, d)), 2.5f, color);
}

static void DrawAnchor(Vector2 a, bool inRange)
{
    // Hollow targeting ring + crosshair in cyan/slate, so anchors can't be
    // mistaken for the solid gold coins.
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
    DrawEllipse((int)pos.x, (int)pos.y, 10.0f * squish, 10.0f, (Color) { 255, 215, 60, 255 });
    DrawEllipseLines((int)pos.x, (int)pos.y, 10.0f * squish, 10.0f, (Color) { 160, 120, 20, 255 });
}

static void DrawPlayer(Player* p)
{
    Rectangle pr = PlayerRect(p->position);
    Vector2 center = PlayerCenter(p);

    float w = PLAYER_W * p->scale.x;
    float h = PLAYER_H * p->scale.y;
    Rectangle draw = { center.x - w * 0.5f, center.y + PLAYER_H * 0.5f - h, w, h };

    Color body = p->grappling ? (Color) { 220, 130, 60, 255 } : (Color) { 200, 60, 60, 255 };
    DrawRectangleRec(draw, body);
    DrawRectangleLinesEx(draw, 2, (Color) { 60, 10, 10, 255 });

    float eyeX = draw.x + draw.width * 0.5f + p->facing * 8.0f;
    DrawCircle((int)eyeX, (int)(draw.y + draw.height * 0.32f), 3, WHITE);

    // simple motion streaks when moving fast, for extra speed sensation
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

//------------------------------------------------------------------------------------
// Main
//------------------------------------------------------------------------------------
int main(void)
{
    InitWindow(SCREEN_W, SCREEN_H, "AI Platformer - Momentum & Grapple");
    SetExitKey(KEY_NULL); // ESC is ours to use for menu navigation, not window close
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
    Rectangle quitButton  = { SCREEN_W / 2.0f - 110, SCREEN_H / 2.0f + 40, 220, 56 };
    float menuTime = 0.0f;
    bool playHoveredPrev = false;
    bool quitHoveredPrev = false;

    // Level select layout: LEVEL_COUNT cards in a row, plus a back button
    const float cardW = 240.0f, cardH = 210.0f, cardGap = 30.0f;
    const float cardsX = (SCREEN_W - (LEVEL_COUNT * cardW + (LEVEL_COUNT - 1) * cardGap)) / 2.0f;
    Rectangle levelCards[LEVEL_COUNT];
    for (int i = 0; i < LEVEL_COUNT; i++)
        levelCards[i] = (Rectangle){ cardsX + i * (cardW + cardGap), SCREEN_H / 2.0f - 90, cardW, cardH };
    Rectangle backButton = { SCREEN_W / 2.0f - 110, SCREEN_H / 2.0f + 160, 220, 56 };
    int hoveredCardPrev = -1;
    bool backHoveredPrev = false;

    while (!WindowShouldClose() && !quitRequested)
    {
        float dt = fminf(GetFrameTime(), 1.0f / 30.0f);

        if (state == STATE_MENU)
        {
            menuTime += dt;

            // Slow auto-pan keeps the parallax hills/clouds drifting instead
            // of sitting frozen behind the menu.
            camera.target.x = menuTime * 35.0f;

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
                SpawnBurst(btnCenter, 16, 200.0f, 0.5f, 3.5f, (Color){ 250, 220, 120, 255 });
                state = STATE_LEVEL_SELECT;
            }
            if (quitClicked) quitRequested = true;

            UpdateParticles(dt);

            BeginDrawing();
            ClearBackground((Color){ 190, 220, 245, 255 });
            DrawParallaxBackground(camera);
            DrawTwinkleStars(menuTime);

            Rectangle panel = { SCREEN_W / 2.0f - 340, SCREEN_H / 2.0f - 220, 680, 420 };
            DrawRectangleRounded(panel, 0.08f, 12, Fade(BLACK, 0.22f));

            const char *title = "AI PLATFORMER";
            int fontSize = 64;
            int tw = MeasureText(title, fontSize);
            float titleY = SCREEN_H / 2.0f - 160.0f + sinf(menuTime * 1.6f) * 5.0f;
            DrawText(title, SCREEN_W / 2 - tw / 2 + 3, (int)titleY + 3, fontSize, Fade(BLACK, 0.35f));
            DrawText(title, SCREEN_W / 2 - tw / 2, (int)titleY, fontSize, (Color){ 255, 236, 160, 255 });

            DrawButton(playButton, "PLAY", 26);
            DrawButton(quitButton, "QUIT", 26);

            DrawParticles();

            const char *controls1 = "A/D or Arrows: run   SPACE: jump (double-jump in air!)   F / Click: grapple";
            const char *controls2 = "While grappling -> W/S or Up/Down: reel in/out    R: restart    ESC: level select";
            int cw1 = MeasureText(controls1, 16);
            int cw2 = MeasureText(controls2, 16);
            DrawText(controls1, SCREEN_W / 2 - cw1 / 2, SCREEN_H / 2 + 120, 16, (Color){ 230, 230, 235, 255 });
            DrawText(controls2, SCREEN_W / 2 - cw2 / 2, SCREEN_H / 2 + 144, 16, (Color){ 230, 230, 235, 255 });

            EndDrawing();
            continue;
        }

        if (state == STATE_LEVEL_SELECT)
        {
            menuTime += dt;
            camera.target.x = menuTime * 35.0f;

            int hoveredCard = -1;
            for (int i = 0; i < LEVEL_COUNT; i++)
                if (CheckCollisionPointRec(GetMousePosition(), levelCards[i])) hoveredCard = i;
            bool backHovered = CheckCollisionPointRec(GetMousePosition(), backButton);
            if ((hoveredCard >= 0 && hoveredCard != hoveredCardPrev) || (backHovered && !backHoveredPrev)) Snd(sndCoin);
            hoveredCardPrev = hoveredCard;
            backHoveredPrev = backHovered;

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
            else if (IsButtonClicked(backButton) || IsKeyPressed(KEY_ESCAPE))
            {
                state = STATE_MENU;
            }

            UpdateParticles(dt);

            BeginDrawing();
            ClearBackground((Color){ 190, 220, 245, 255 });
            DrawParallaxBackground(camera);
            DrawTwinkleStars(menuTime);

            Rectangle panel = { SCREEN_W / 2.0f - 570, SCREEN_H / 2.0f - 250, 1140, 520 };
            DrawRectangleRounded(panel, 0.06f, 12, Fade(BLACK, 0.22f));

            const char *title = "SELECT LEVEL";
            int tw = MeasureText(title, 56);
            float titleY = SCREEN_H / 2.0f - 200.0f + sinf(menuTime * 1.6f) * 4.0f;
            DrawText(title, SCREEN_W / 2 - tw / 2 + 3, (int)titleY + 3, 56, Fade(BLACK, 0.35f));
            DrawText(title, SCREEN_W / 2 - tw / 2, (int)titleY, 56, (Color){ 255, 236, 160, 255 });

            for (int i = 0; i < LEVEL_COUNT; i++)
                DrawLevelCard(levelCards[i], i + 1, levelNames[i], bestTimes[i], levelPreviews[i].texture);
            DrawButton(backButton, "BACK", 26);

            char hint[64];
            snprintf(hint, sizeof(hint), "Click a level or press 1-%d    ESC: back", LEVEL_COUNT);
            int hw = MeasureText(hint, 16);
            DrawText(hint, SCREEN_W / 2 - hw / 2, SCREEN_H / 2 + 235, 16, (Color){ 230, 230, 235, 255 });

            DrawParticles();
            EndDrawing();
            continue;
        }

        if (IsKeyPressed(KEY_ESCAPE))
        {
            // Leave the level back to the level select screen. Falls through
            // so this frame still finishes its EndDrawing (which polls input);
            // skipping it would leave ESC "pressed" for the next screen.
            state = STATE_LEVEL_SELECT;
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

            // ---- Horizontal input / momentum ----
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
                    // Pumping the swing: no run-speed cap while hooked.
                    player.velocity.x += moveDir * accel * dt;
                }
                else if (fabsf(vx) <= MAX_RUN_SPEED)
                {
                    // Normal running: accelerate up to the cap, never past it.
                    player.velocity.x = Clamp(vx + moveDir * accel * dt, -MAX_RUN_SPEED, MAX_RUN_SPEED);
                }
                else if (vx * moveDir < 0.0f)
                {
                    // Above the cap but pushing against momentum: braking is allowed.
                    player.velocity.x += moveDir * accel * dt;
                }
                else if (player.onGround)
                {
                    // Above the cap on the ground while pushing the same way:
                    // ease back down to the cap instead of snapping to it.
                    player.velocity.x = copysignf(fmaxf(fabsf(vx) - friction * dt, MAX_RUN_SPEED), vx);
                }
                // else: airborne above the cap, pushing the same way -- keep the
                // momentum (no extra acceleration, no clamp).
                // occasional running dust while grounded and moving fast
                if (player.onGround && GetRandomValue(0, 100) < 12)
                {
                    Vector2 feet = { player.position.x + PLAYER_W * 0.5f, player.position.y + PLAYER_H };
                    SpawnParticle(feet, (Vector2) { -moveDir * 40.0f, -30.0f }, 0.3f, 2.5f, (Color) { 210, 200, 170, 200 });
                }
            }
            else if (!(player.grappling && !player.onGround))
            {
                // No air friction while hooked: swing momentum should carry.
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

            // ---- Coyote time & jump buffer bookkeeping ----
            if (player.onGround) player.coyoteTimer = COYOTE_TIME;
            else player.coyoteTimer -= dt;

            if (IsKeyPressed(KEY_SPACE)) player.jumpBufferTimer = JUMP_BUFFER_TIME;
            else player.jumpBufferTimer -= dt;

            // ---- Jump (grounded, coyote-time, or buffered) ----
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
            // ---- Double jump: available once per airtime, refreshed by grounding or grappling ----
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

            // ---- Grapple fire / release ----
            if (IsKeyPressed(KEY_F) || IsMouseButtonPressed(MOUSE_BUTTON_LEFT))
            {
                if (player.grappling) player.grappling = false;
                else TryFireGrapple(&player);
            }

            // ---- Gravity ----
            player.velocity.y += GRAVITY * dt;
            if (!player.grappling) player.velocity.y = fminf(player.velocity.y, MAX_FALL_SPEED);

            // ---- Physics step ----
            MoveAndCollide(&player, dt);
            UpdateGrapple(&player, dt);

            // ---- Squash & stretch relaxes back to normal each frame ----
            player.scale.x = Lerp(player.scale.x, 1.0f, 1.0f - expf(-14.0f * dt));
            player.scale.y = Lerp(player.scale.y, 1.0f, 1.0f - expf(-14.0f * dt));

            // ---- Coins ----
            CheckCoins(&player);

            // ---- Enemies ----
            UpdateEnemies(dt);

            // ---- Hazards ----
            if (TouchesAnySpike(&player))
            {
                Snd(sndDeath);
                Shake(8.0f, 0.2f);
                SpawnBurst(PlayerCenter(&player), 16, 220.0f, 0.5f, 4.0f, (Color) { 220, 40, 40, 255 });
                StartLevel(currentLevel, &player); // dying restarts the run: timer, coins, ghost recording
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

            // ---- Ghost recording ----
            RecordGhost(&player);

            // ---- Goal ----
            if (CheckCollisionRecs(PlayerRect(player.position), goalRect))
            {
                won = true;
                Snd(sndWin);
                Shake(10.0f, 0.3f);
                SpawnBurst(PlayerCenter(&player), 40, 260.0f, 0.8f, 4.5f, (Color) { 255, 220, 90, 255 });
                if (bestTimes[currentLevel] < 0.0f || levelTime < bestTimes[currentLevel])
                {
                    bestTimes[currentLevel] = levelTime;
                    SaveGhost(currentLevel); // new personal best: this run becomes the ghost
                }
            }
        }

        UpdateParticles(dt);
        if (shakeTime > 0.0f) shakeTime -= dt; else shakeMagnitude = 0.0f;

        // ---- Camera ----
        // ---- Speed intensity (0..1 based on horizontal velocity) ----
        float rawSpeed = Vector2Length(player.velocity);
        float targetIntensity = (rawSpeed - SPEED_BLUR_MIN) / (SPEED_BLUR_MAX - SPEED_BLUR_MIN);
        if (targetIntensity < 0.0f) targetIntensity = 0.0f;
        if (targetIntensity > 1.0f) targetIntensity = 1.0f;
        // Only consider horizontal + downward speed for the "rush" feel
        float horizSpeed = fabsf(player.velocity.x);
        float horizIntensity = (horizSpeed - 200.0f) / 500.0f;
        if (horizIntensity < 0.0f) horizIntensity = 0.0f;
        if (horizIntensity > 1.0f) horizIntensity = 1.0f;

        float finalIntensity = targetIntensity > horizIntensity ? targetIntensity : horizIntensity;

        // Smooth intensity for visual filters
        speedIntensity = Lerp(speedIntensity, finalIntensity, 1.0f - expf(-8.0f * dt));

        // Smooth display speed for MPH (MPH = world units/sec * scale)
        displaySpeed = Lerp(displaySpeed, rawSpeed, 1.0f - expf(-6.0f * dt));

        // ---- Camera with lead/lag ----
        Vector2 playerCenter = PlayerCenter(&player);

        // Camera leads in movement direction, creating drag/anticipation feel
        Vector2 leadOffset = {
            player.velocity.x * CAMERA_LEAD_FACTOR * 0.25f,
            player.velocity.y * CAMERA_LEAD_FACTOR * 0.12f  // less vertical lead
        };

        // When starting/stopping, camera lags behind
        Vector2 desiredTarget = Vector2Add(playerCenter, leadOffset);

        // Smooth camera position
        Vector2 diff = Vector2Subtract(desiredTarget, cameraSmooth);
        cameraSmooth = Vector2Add(cameraSmooth, Vector2Scale(diff, 1.0f - expf(-CAMERA_LAG_SPEED * dt)));

        camera.target = cameraSmooth;

        float halfW = SCREEN_W / 2.0f;
        float halfH = SCREEN_H / 2.0f;
        camera.target.x = Clamp(camera.target.x, halfW, worldWidth - halfW);
        camera.target.y = Clamp(camera.target.y, worldTopY + halfH, worldBottomY - halfH);

        // Camera zoom pulls back slightly at high speed for wider view
        float targetZoom = 1.0f - speedIntensity * 0.08f;
        camera.zoom = Lerp(camera.zoom, targetZoom, 1.0f - expf(-6.0f * dt));

        Vector2 camOffset = { SCREEN_W / 2.0f, SCREEN_H / 2.0f };
        if (shakeMagnitude > 0.01f)
        {
            camOffset.x += (float)GetRandomValue(-100, 100) / 100.0f * shakeMagnitude;
            camOffset.y += (float)GetRandomValue(-100, 100) / 100.0f * shakeMagnitude;
            shakeMagnitude *= 0.9f;
        }
        // High-speed micro-jitter for intensity
        if (speedIntensity > 0.5f)
        {
            float jitter = (speedIntensity - 0.5f) * 4.0f;
            camOffset.x += (float)GetRandomValue(-100, 100) / 100.0f * jitter;
            camOffset.y += (float)GetRandomValue(-100, 100) / 100.0f * jitter * 0.5f;
        }
        camera.offset = camOffset;

        // ---- Spawn speed streaks based on player velocity ----
        if (speedIntensity > 0.1f)
        {
            int streakCount = (int)(speedIntensity * 4.0f);
            Vector2 sc = { SCREEN_W / 2.0f, SCREEN_H / 2.0f };
            for (int i = 0; i < streakCount; i++)
                SpawnSpeedStreak(player.velocity, sc);
        }
        UpdateSpeedStreaks(dt);

        // ---- Update motion trail ----
        UpdateTrailGhosts(&player, dt);

        // ---- Draw ----
        BeginDrawing();
        ClearBackground((Color) { 190, 220, 245, 255 });

        // Apply camera zoom to background too for consistency
        DrawParallaxBackground(camera);

        BeginMode2D(camera);

        // Draw trail ghosts before player
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
            DrawLineEx(PlayerCenter(&player), player.grappleAnchor, 2.5f, (Color) { 40, 40, 40, 255 });
        }
        else
        {
            // Aim indicator: dotted line to the anchor a click would hook.
            int aimed = FindBestAnchor(&player);
            if (aimed >= 0)
                DrawDottedLine(PlayerCenter(&player), anchors[aimed], (float)GetTime(), (Color) { 70, 220, 255, 220 });
        }

        DrawParticles();
        DrawGhost(currentLevel, levelTime);
        DrawPlayer(&player);

        EndMode2D();

        // ---- Speed visual filters (drawn in screen space) ----
        DrawRadialSpeedLines(player.velocity, speedIntensity);
        DrawSpeedStreaks(camera);
        DrawChromaticAberration(speedIntensity);
        DrawSpeedVignette(speedIntensity);

        // ---- HUD ----

        // ---- HUD ----
        DrawRectangle(0, 0, SCREEN_W, 84, Fade(BLACK, 0.35f));
        DrawText("A/D or Arrows: run   SPACE: jump (double-jump in air!)   F / Click: grapple",
            16, 8, 18, RAYWHITE);
        DrawText("While grappling -> W/S or Up/Down: reel in/out    R: restart    ESC: level select",
            16, 32, 18, RAYWHITE);
        DrawText("Hit a wall hard enough and you'll bounce off it",
            16, 56, 16, (Color) { 220, 220, 220, 255 });

        char hud[128];
        snprintf(hud, sizeof(hud), "Coins: %d/%d      Time: %05.2fs", coinsCollected, coinCount, levelTime);
        int hw = MeasureText(hud, 22);
        DrawText(hud, SCREEN_W - hw - 16, 12, 22, (Color) { 255, 230, 120, 255 });

        // ---- MPH counter (bottom center) ----
        {
            float mph = displaySpeed * MPH_SCALE;

            // Color shifts from white -> yellow -> orange -> red as speed increases
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

            // Background panel for readability
            DrawRectangle(SCREEN_W / 2 - mphW / 2 - 16, SCREEN_H - 60, mphW + 32, 48,
                Fade(BLACK, 0.45f + speedIntensity * 0.3f));

            // Slight scale pulse at high speed
            float pulse = 1.0f + speedIntensity * 0.08f * sinf(levelTime * 20.0f);
            int fontSize = (int)(36 * pulse);

            DrawText(mphText, SCREEN_W / 2 - mphW / 2, SCREEN_H - 54, fontSize, mphColor);

            // Speed bar underneath
            float barW = 300.0f;
            float barH = 6.0f;
            float barX = SCREEN_W / 2 - barW / 2;
            float barY = SCREEN_H - 16;

            DrawRectangle((int)barX, (int)barY, (int)barW, (int)barH, Fade(BLACK, 0.5f));
            DrawRectangle((int)barX, (int)barY, (int)(barW * speedIntensity), (int)barH, mphColor);

            // Tick marks at thirds
            for (int i = 1; i < 3; i++)
            {
                int tx = (int)(barX + barW * i / 3.0f);
                DrawRectangle(tx, (int)barY - 2, 1, (int)barH + 4, Fade(WHITE, 0.4f));
            }

            // "SPEED" label on the side
            DrawText("SPEED", (int)(barX - 56), (int)(barY - 4), 14, Fade(WHITE, 0.6f));
        }
        if (bestTimes[currentLevel] > 0.0f)
        {
            char best[64];
            snprintf(best, sizeof(best), "Best: %05.2fs", bestTimes[currentLevel]);
            int bw = MeasureText(best, 18);
            DrawText(best, SCREEN_W - bw - 16, 38, 18, (Color) { 200, 220, 255, 255 });
        }

        if (won)
        {
            const char* msg = "LEVEL COMPLETE!  R: replay   ESC: level select";
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
