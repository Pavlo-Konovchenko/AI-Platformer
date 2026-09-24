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

#define MAX_SOLIDS  48
#define MAX_SPIKES  24
#define MAX_ANCHORS 12
#define MAX_COINS   32
#define MAX_PARTICLES 300
#define LEVEL_COUNT 3

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
static float bestTimes[LEVEL_COUNT] = { -1.0f, -1.0f, -1.0f };
static int currentLevel = 0;
static const char *levelNames[LEVEL_COUNT] = { "Momentum Run", "Spike Alley", "Sky Islands" };

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

static void BuildLevel(int index)
{
    solidCount = spikeCount = anchorCount = coinCount = 0;
    switch (index)
    {
        case 1:  BuildLevel2(); break;
        case 2:  BuildLevel3(); break;
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

    float s = fminf((float)PREVIEW_W / worldWidth, (float)PREVIEW_H / (maxY - minY));
    Camera2D cam = { 0 };
    cam.offset = (Vector2){ PREVIEW_W / 2.0f, PREVIEW_H / 2.0f };
    cam.target = (Vector2){ worldWidth / 2.0f, (minY + maxY) / 2.0f };
    cam.zoom = s;

    levelPreviews[index] = LoadRenderTexture(PREVIEW_W, PREVIEW_H);
    SetTextureFilter(levelPreviews[index].texture, TEXTURE_FILTER_BILINEAR);

    BeginTextureMode(levelPreviews[index]);
    DrawRectangleGradientV(0, 0, PREVIEW_W, PREVIEW_H, (Color){ 150, 200, 240, 255 }, (Color){ 225, 240, 250, 255 });
    BeginMode2D(cam);

    for (int i = 0; i < solidCount; i++)
    {
        Color c;
        switch (solids[i].type)
        {
            case SOLID_FLOATING: c = (Color){ 120, 160, 220, 255 }; break;
            case SOLID_WALL:     c = (Color){ 90, 90, 100, 255 };  break;
            default:             c = (Color){ 80, 130, 80, 255 };  break;
        }
        DrawRectangleRec(solids[i].rect, c);
    }
    for (int i = 0; i < spikeCount; i++)
    {
        Rectangle r = spikes[i];
        DrawTriangle((Vector2){ r.x, r.y + r.height }, (Vector2){ r.x + r.width * 0.5f, r.y - 30.0f },
                     (Vector2){ r.x + r.width, r.y + r.height }, (Color){ 190, 40, 40, 255 });
    }
    for (int i = 0; i < coinCount; i++) DrawCircleV(coins[i].pos, 26.0f, (Color){ 255, 215, 60, 255 });
    for (int i = 0; i < anchorCount; i++) DrawCircleV(anchors[i], 30.0f, (Color){ 90, 110, 170, 255 });
    DrawRectangle((int)goalRect.x - 10, (int)goalRect.y - 30, (int)goalRect.width + 60, (int)goalRect.height + 30, (Color){ 60, 190, 90, 255 });
    DrawCircleV((Vector2){ spawnPoint.x + PLAYER_W * 0.5f, spawnPoint.y + PLAYER_H * 0.5f }, 34.0f, (Color){ 200, 60, 60, 255 });

    EndMode2D();
    EndTextureMode();
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
    const float cardW = 260.0f, cardH = 210.0f, cardGap = 40.0f;
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

            Rectangle panel = { SCREEN_W / 2.0f - 500, SCREEN_H / 2.0f - 250, 1000, 520 };
            DrawRectangleRounded(panel, 0.06f, 12, Fade(BLACK, 0.22f));

            const char *title = "SELECT LEVEL";
            int tw = MeasureText(title, 56);
            float titleY = SCREEN_H / 2.0f - 200.0f + sinf(menuTime * 1.6f) * 4.0f;
            DrawText(title, SCREEN_W / 2 - tw / 2 + 3, (int)titleY + 3, 56, Fade(BLACK, 0.35f));
            DrawText(title, SCREEN_W / 2 - tw / 2, (int)titleY, 56, (Color){ 255, 236, 160, 255 });

            for (int i = 0; i < LEVEL_COUNT; i++)
                DrawLevelCard(levelCards[i], i + 1, levelNames[i], bestTimes[i], levelPreviews[i].texture);
            DrawButton(backButton, "BACK", 26);

            const char *hint = "Click a level or press 1-3    ESC: back";
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

            // ---- Hazards ----
            if (TouchesAnySpike(&player))
            {
                Snd(sndDeath);
                Shake(8.0f, 0.2f);
                SpawnBurst(PlayerCenter(&player), 16, 220.0f, 0.5f, 4.0f, (Color) { 220, 40, 40, 255 });
                RespawnPlayer(&player);
            }
            if (player.position.y > WORLD_DEATH_Y)
            {
                Snd(sndDeath);
                RespawnPlayer(&player);
            }

            // ---- Goal ----
            if (CheckCollisionRecs(PlayerRect(player.position), goalRect))
            {
                won = true;
                Snd(sndWin);
                Shake(10.0f, 0.3f);
                SpawnBurst(PlayerCenter(&player), 40, 260.0f, 0.8f, 4.5f, (Color) { 255, 220, 90, 255 });
                if (bestTimes[currentLevel] < 0.0f || levelTime < bestTimes[currentLevel])
                    bestTimes[currentLevel] = levelTime;
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
    ShutdownGameAudio();
    CloseWindow();
    return 0;
}
