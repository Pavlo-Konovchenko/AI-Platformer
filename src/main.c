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
static float bestTime = -1.0f;

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
        SpawnParticle(pos, v, life * (0.6f + 0.4f * (GetRandomValue(0,100)/100.0f)), size, color);
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
    short *data = (short *)malloc(sizeof(short) * frameCount);

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

    sndJump       = MakeTone(420.0f, 0.12f,  260.0f, true);
    sndDoubleJump = MakeTone(560.0f, 0.14f,  420.0f, true);
    sndLand       = MakeTone(140.0f, 0.08f, -60.0f,  true);
    sndGrapple    = MakeTone(700.0f, 0.10f,  180.0f, true);
    sndWallBounce = MakeTone(260.0f, 0.10f,  120.0f, true);
    sndCoin       = MakeTone(900.0f, 0.14f,  700.0f, false);
    sndWin        = MakeTone(600.0f, 0.55f,  500.0f, false);
    sndDeath      = MakeTone(300.0f, 0.30f, -220.0f, true);
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
static void BuildLevel(void)
{
    solidCount = spikeCount = anchorCount = coinCount = 0;

    // --- Section 1: start, simple gap ---
    AddSolid(   0, 650, 520, 100, SOLID_GROUND);
    AddCoin(260, 580);
    // pit: 520 - 660
    AddSolid( 660, 650, 380, 100, SOLID_GROUND);
    AddCoin(840, 580);

    // --- Section 2: low wall to jump over, then spikes on the ground ---
    AddSolid(1040, 460, 40, 290, SOLID_WALL);
    AddSolid(1080, 650, 320, 100, SOLID_GROUND);
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
}

//------------------------------------------------------------------------------------
// Collision helpers
//------------------------------------------------------------------------------------
static Rectangle PlayerRect(Vector2 pos)
{
    return (Rectangle){ pos.x, pos.y, PLAYER_W, PLAYER_H };
}

static Vector2 PlayerCenter(Player *p)
{
    return (Vector2){ p->position.x + PLAYER_W * 0.5f, p->position.y + PLAYER_H * 0.5f };
}

static void MoveAndCollide(Player *p, float dt)
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
                SpawnBurst(PlayerCenter(p), 10, 220.0f, 0.35f, 4.0f, (Color){ 230, 230, 240, 255 });
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
        SpawnBurst(feet, 8, 140.0f, 0.4f, 3.5f, (Color){ 210, 200, 170, 255 });
        p->usedDoubleJump = false;
        p->coyoteTimer = 0.0f;
    }
}

// The grapple constraint repositions the player directly, which can leave
// the body edge-on into a wall the rope circle happened to pass through.
// This pushes the player back out along the shallowest overlap axis.
static void ResolveSolidOverlap(Player *p)
{
    Rectangle pr = PlayerRect(p->position);
    for (int i = 0; i < solidCount; i++)
    {
        Rectangle r = solids[i].rect;
        if (!CheckCollisionRecs(pr, r)) continue;

        float overlapLeft = (pr.x + pr.width) - r.x;
        float overlapRight = (r.x + r.width) - pr.x;
        float overlapTop = (pr.y + pr.height) - r.y;
        float overlapBottom = (r.y + r.height) - pr.y;

        float pushX = (overlapLeft < overlapRight) ? -overlapLeft : overlapRight;
        float pushY = (overlapTop < overlapBottom) ? -overlapTop : overlapBottom;

        if (fabsf(pushX) < fabsf(pushY))
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

static bool TouchesAnySpike(Player *p)
{
    Rectangle pr = PlayerRect(p->position);
    for (int i = 0; i < spikeCount; i++)
        if (CheckCollisionRecs(pr, spikes[i])) return true;
    return false;
}

static void CheckCoins(Player *p)
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
            SpawnBurst(coins[i].pos, 12, 180.0f, 0.5f, 3.0f, (Color){ 255, 215, 60, 255 });
        }
    }
}

static void RespawnPlayer(Player *p)
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
// Grapple
//------------------------------------------------------------------------------------

// Casts a ray from origin along the unit vector dir and returns the distance
// to the nearest solid it hits, capped at maxDist. Standard slab method.
// Stops the grapple rope from reeling the player straight through a wall
// that sits between the anchor and them.
static float RaycastSolids(Vector2 origin, Vector2 dir, float maxDist)
{
    float nearest = maxDist;
    for (int i = 0; i < solidCount; i++)
    {
        Rectangle r = solids[i].rect;
        float tmin = 0.0f;
        float tmax = nearest;

        if (fabsf(dir.x) < 1e-6f)
        {
            if (origin.x < r.x || origin.x > r.x + r.width) continue;
        }
        else
        {
            float t1 = (r.x - origin.x) / dir.x;
            float t2 = (r.x + r.width - origin.x) / dir.x;
            if (t1 > t2) { float tmp = t1; t1 = t2; t2 = tmp; }
            if (t1 > tmin) tmin = t1;
            if (t2 < tmax) tmax = t2;
            if (tmin > tmax) continue;
        }

        if (fabsf(dir.y) < 1e-6f)
        {
            if (origin.y < r.y || origin.y > r.y + r.height) continue;
        }
        else
        {
            float t1 = (r.y - origin.y) / dir.y;
            float t2 = (r.y + r.height - origin.y) / dir.y;
            if (t1 > t2) { float tmp = t1; t1 = t2; t2 = tmp; }
            if (t1 > tmin) tmin = t1;
            if (t2 < tmax) tmax = t2;
            if (tmin > tmax) continue;
        }

        if (tmin > 0.0001f && tmin < nearest) nearest = tmin;
    }
    return nearest;
}

static void TryFireGrapple(Player *p)
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
    if (best >= 0)
    {
        p->grappling = true;
        p->grappleAnchor = anchors[best];
        p->ropeLength = Vector2Distance(center, anchors[best]);
        if (p->ropeLength < GRAPPLE_MIN_LEN) p->ropeLength = GRAPPLE_MIN_LEN;
        p->usedDoubleJump = false; // grappling refreshes the double jump for extra chaining fun
        Snd(sndGrapple);
        SpawnBurst(anchors[best], 10, 160.0f, 0.4f, 3.0f, (Color){ 250, 210, 60, 255 });
    }
}

static void UpdateGrapple(Player *p, float dt)
{
    if (!p->grappling) return;

    if (IsKeyDown(KEY_UP) || IsKeyDown(KEY_W))
    {
        p->ropeLength -= GRAPPLE_REEL_SPD * dt;
        if (p->ropeLength < GRAPPLE_MIN_LEN) p->ropeLength = GRAPPLE_MIN_LEN;
    }
    if (IsKeyDown(KEY_DOWN) || IsKeyDown(KEY_S))
    {
        p->ropeLength += GRAPPLE_REEL_SPD * dt;
        if (p->ropeLength > GRAPPLE_RANGE) p->ropeLength = GRAPPLE_RANGE;
    }

    Vector2 center = PlayerCenter(p);
    Vector2 diff = Vector2Subtract(center, p->grappleAnchor);
    float dist = Vector2Length(diff);

    if (dist > p->ropeLength && dist > 0.0001f)
    {
        Vector2 dir = Vector2Scale(diff, 1.0f / dist);

        float wallDist = RaycastSolids(p->grappleAnchor, dir, dist);
        if (wallDist < p->ropeLength) p->ropeLength = wallDist;

        Vector2 newCenter = Vector2Add(p->grappleAnchor, Vector2Scale(dir, p->ropeLength));
        p->position.x = newCenter.x - PLAYER_W * 0.5f;
        p->position.y = newCenter.y - PLAYER_H * 0.5f;

        ResolveSolidOverlap(p);

        float radialSpeed = Vector2DotProduct(p->velocity, dir);
        if (radialSpeed > 0.0f)
        {
            p->velocity = Vector2Subtract(p->velocity, Vector2Scale(dir, radialSpeed));
        }

        Vector2 tangent = (Vector2){ -dir.y, dir.x };
        float tangentSpeed = Vector2DotProduct(p->velocity, tangent);
        float sign = (tangentSpeed >= 0) ? 1.0f : -1.0f;
        p->velocity = Vector2Add(p->velocity, Vector2Scale(tangent, sign * GRAPPLE_PULL_ACC * dt * 0.15f));
    }

    // A steady trail of little sparks along the rope makes the swing read
    // as "powered" rather than just a static line.
    if (GetRandomValue(0, 100) < 40)
    {
        float t = (float)GetRandomValue(20, 80) / 100.0f;
        Vector2 pt = Vector2Lerp(center, p->grappleAnchor, t);
        SpawnParticle(pt, (Vector2){ 0, -20 }, 0.25f, 2.0f, (Color){ 255, 240, 180, 200 });
    }
}

//------------------------------------------------------------------------------------
// Drawing
//------------------------------------------------------------------------------------
static void DrawParallaxBackground(Camera2D camera)
{
    // Sky gradient
    DrawRectangleGradientV(0, 0, SCREEN_W, SCREEN_H, (Color){ 150, 200, 240, 255 }, (Color){ 225, 240, 250, 255 });

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
    DrawRectangleLinesEx(s->rect, 2, (Color){ 30, 30, 30, 255 });
    if (s->type == SOLID_GROUND)
    {
        DrawRectangle((int)s->rect.x, (int)s->rect.y, (int)s->rect.width, 8, (Color){ 110, 190, 90, 255 });
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
        DrawTriangle(p1, p2, p3, (Color){ 190, 40, 40, 255 });
        DrawTriangleLines(p1, p2, p3, (Color){ 90, 10, 10, 255 });
    }
}

static void DrawAnchor(Vector2 a, bool inRange)
{
    DrawCircleV(a, 9, inRange ? (Color){ 250, 210, 60, 255 } : (Color){ 90, 110, 170, 255 });
    DrawCircleLines((int)a.x, (int)a.y, 9, (Color){ 20, 20, 30, 255 });
    DrawCircleLines((int)a.x, (int)a.y, (int)GRAPPLE_RANGE, (Color){ 90, 110, 170, 40 });
}

static void DrawCoin(Coin *c, float t)
{
    if (c->collected) return;
    float bobY = sinf(t * 3.0f + c->bob) * 5.0f;
    float squish = 0.55f + 0.45f * fabsf(cosf(t * 2.2f + c->bob));
    Vector2 pos = { c->pos.x, c->pos.y + bobY };
    DrawEllipse((int)pos.x, (int)pos.y, 10.0f * squish, 10.0f, (Color){ 255, 215, 60, 255 });
    DrawEllipseLines((int)pos.x, (int)pos.y, 10.0f * squish, 10.0f, (Color){ 160, 120, 20, 255 });
}

static void DrawPlayer(Player *p)
{
    Rectangle pr = PlayerRect(p->position);
    Vector2 center = PlayerCenter(p);

    float w = PLAYER_W * p->scale.x;
    float h = PLAYER_H * p->scale.y;
    Rectangle draw = { center.x - w * 0.5f, center.y + PLAYER_H * 0.5f - h, w, h };

    Color body = p->grappling ? (Color){ 220, 130, 60, 255 } : (Color){ 200, 60, 60, 255 };
    DrawRectangleRec(draw, body);
    DrawRectangleLinesEx(draw, 2, (Color){ 60, 10, 10, 255 });

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
                     (int)(pr.x - p->facing * (2 + i * 8)),  (int)(pr.y + 10 + i * 6),
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
    SetTargetFPS(60);
    InitGameAudio();

    BuildLevel();

    Player player = { 0 };
    RespawnPlayer(&player);
    player.facing = 1.0f;
    coinsCollected = 0;

    Camera2D camera = { 0 };
    camera.offset = (Vector2){ SCREEN_W / 2.0f, SCREEN_H / 2.0f };
    camera.zoom = 1.0f;

    bool won = false;

    while (!WindowShouldClose())
    {
        float dt = GetFrameTime();
        if (dt > 1.0f / 30.0f) dt = 1.0f / 30.0f;

        if (IsKeyPressed(KEY_R))
        {
            RespawnPlayer(&player);
            won = false;
            levelTime = 0.0f;
            coinsCollected = 0;
            for (int i = 0; i < coinCount; i++) coins[i].collected = false;
        }

        if (!won)
        {
            levelTime += dt;

            // ---- Horizontal input / momentum ----
            float moveDir = 0.0f;
            if (IsKeyDown(KEY_RIGHT) || IsKeyDown(KEY_D)) moveDir += 1.0f;
            if (IsKeyDown(KEY_LEFT)  || IsKeyDown(KEY_A)) moveDir -= 1.0f;
            if (moveDir != 0.0f) player.facing = moveDir;

            float accel = player.onGround ? GROUND_ACCEL : AIR_ACCEL;
            float friction = player.onGround ? GROUND_FRICTION : AIR_FRICTION;

            if (moveDir != 0.0f)
            {
                player.velocity.x += moveDir * accel * dt;
                if (!player.grappling)
                {
                    if (player.velocity.x > MAX_RUN_SPEED) player.velocity.x = MAX_RUN_SPEED;
                    if (player.velocity.x < -MAX_RUN_SPEED) player.velocity.x = -MAX_RUN_SPEED;
                }
                // occasional running dust while grounded and moving fast
                if (player.onGround && GetRandomValue(0, 100) < 12)
                {
                    Vector2 feet = { player.position.x + PLAYER_W * 0.5f, player.position.y + PLAYER_H };
                    SpawnParticle(feet, (Vector2){ -moveDir * 40.0f, -30.0f }, 0.3f, 2.5f, (Color){ 210, 200, 170, 200 });
                }
            }
            else
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
                SpawnBurst(feet, 6, 120.0f, 0.3f, 2.5f, (Color){ 210, 200, 170, 220 });
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
                SpawnBurst(PlayerCenter(&player), 14, 160.0f, 0.35f, 3.0f, (Color){ 180, 220, 255, 255 });
            }

            if (IsKeyReleased(KEY_SPACE) && player.velocity.y < 0.0f)
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
            if (player.velocity.y > MAX_FALL_SPEED) player.velocity.y = MAX_FALL_SPEED;

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
                SpawnBurst(PlayerCenter(&player), 16, 220.0f, 0.5f, 4.0f, (Color){ 220, 40, 40, 255 });
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
                SpawnBurst(PlayerCenter(&player), 40, 260.0f, 0.8f, 4.5f, (Color){ 255, 220, 90, 255 });
                if (bestTime < 0.0f || levelTime < bestTime) bestTime = levelTime;
            }
        }

        UpdateParticles(dt);
        if (shakeTime > 0.0f) shakeTime -= dt; else shakeMagnitude = 0.0f;

        // ---- Camera ----
        camera.target = PlayerCenter(&player);
        float halfW = SCREEN_W / 2.0f;
        float halfH = SCREEN_H / 2.0f;
        if (camera.target.x < halfW) camera.target.x = halfW;
        if (camera.target.x > worldWidth - halfW) camera.target.x = worldWidth - halfW;
        if (camera.target.y < worldTopY + halfH) camera.target.y = worldTopY + halfH;
        if (camera.target.y > worldBottomY - halfH) camera.target.y = worldBottomY - halfH;

        Vector2 camOffset = { SCREEN_W / 2.0f, SCREEN_H / 2.0f };
        if (shakeMagnitude > 0.01f)
        {
            camOffset.x += (float)GetRandomValue(-100, 100) / 100.0f * shakeMagnitude;
            camOffset.y += (float)GetRandomValue(-100, 100) / 100.0f * shakeMagnitude;
            shakeMagnitude *= 0.9f;
        }
        camera.offset = camOffset;

        // ---- Draw ----
        BeginDrawing();
        ClearBackground((Color){ 190, 220, 245, 255 });

        DrawParallaxBackground(camera);

        BeginMode2D(camera);

        for (int i = 0; i < solidCount; i++) DrawSolid(&solids[i]);
        for (int i = 0; i < spikeCount; i++) DrawSpike(spikes[i]);
        for (int i = 0; i < coinCount; i++) DrawCoin(&coins[i], levelTime);

        Vector2 pc = PlayerCenter(&player);
        for (int i = 0; i < anchorCount; i++)
        {
            bool inRange = Vector2Distance(pc, anchors[i]) <= GRAPPLE_RANGE;
            DrawAnchor(anchors[i], inRange);
        }

        DrawRectangleRec(goalRect, (Color){ 230, 230, 230, 255 });
        DrawTriangle((Vector2){ goalRect.x + goalRect.width, goalRect.y },
                     (Vector2){ goalRect.x + goalRect.width, goalRect.y + 22 },
                     (Vector2){ goalRect.x + goalRect.width + 34, goalRect.y + 11 },
                     (Color){ 60, 190, 90, 255 });

        if (player.grappling)
        {
            DrawLineEx(PlayerCenter(&player), player.grappleAnchor, 2.5f, (Color){ 40, 40, 40, 255 });
        }

        DrawParticles();
        DrawPlayer(&player);

        EndMode2D();

        // ---- HUD ----
        DrawRectangle(0, 0, SCREEN_W, 84, Fade(BLACK, 0.35f));
        DrawText("A/D or Arrows: run   SPACE: jump (double-jump in air!)   F / Click: grapple",
                 16, 8, 18, RAYWHITE);
        DrawText("While grappling -> W/S or Up/Down: reel in/out    R: restart level",
                 16, 32, 18, RAYWHITE);
        DrawText("Hit a wall hard enough and you'll bounce off it",
                 16, 56, 16, (Color){ 220, 220, 220, 255 });

        char hud[128];
        snprintf(hud, sizeof(hud), "Coins: %d/%d      Time: %05.2fs", coinsCollected, coinCount, levelTime);
        int hw = MeasureText(hud, 22);
        DrawText(hud, SCREEN_W - hw - 16, 12, 22, (Color){ 255, 230, 120, 255 });
        if (bestTime > 0.0f)
        {
            char best[64];
            snprintf(best, sizeof(best), "Best: %05.2fs", bestTime);
            int bw = MeasureText(best, 18);
            DrawText(best, SCREEN_W - bw - 16, 38, 18, (Color){ 200, 220, 255, 255 });
        }

        if (won)
        {
            const char *msg = "LEVEL COMPLETE! Press R to play again.";
            int w = MeasureText(msg, 40);
            DrawRectangle(SCREEN_W / 2 - w / 2 - 20, SCREEN_H / 2 - 50, w + 40, 100, Fade(BLACK, 0.6f));
            DrawText(msg, SCREEN_W / 2 - w / 2, SCREEN_H / 2 - 30, 40, (Color){ 250, 220, 80, 255 });
            char sub[96];
            snprintf(sub, sizeof(sub), "Time: %05.2fs   Coins: %d/%d", levelTime, coinsCollected, coinCount);
            int sw = MeasureText(sub, 20);
            DrawText(sub, SCREEN_W / 2 - sw / 2, SCREEN_H / 2 + 18, 20, RAYWHITE);
        }

        DrawFPS(SCREEN_W - 90, SCREEN_H - 24);

        EndDrawing();
    }

    ShutdownGameAudio();
    CloseWindow();
    return 0;
}
