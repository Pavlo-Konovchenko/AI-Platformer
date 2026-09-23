#include "raylib.h"
#include "raymath.h"
#include <math.h>
#include <string.h>

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
#define JUMP_CUT_MULT     0.45f    // short-hop when space released early

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
} Player;

static Solid   solids[MAX_SOLIDS];
static int     solidCount = 0;
static Rectangle spikes[MAX_SPIKES];
static int     spikeCount = 0;
static Vector2 anchors[MAX_ANCHORS];
static int     anchorCount = 0;
static Rectangle goalRect;
static Vector2 spawnPoint;
static float   worldWidth = 6000.0f;
static float   worldTopY = -60.0f;   // highest point the camera should show
static float   worldBottomY = 1000.0f; // lowest point the camera should show

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

//------------------------------------------------------------------------------------
// Level layout
// Ground baseline top = 650. Gaps between ground segments are pits.
// A handful of floating platforms and tall walls require the grapple to cross.
//------------------------------------------------------------------------------------
static void BuildLevel(void)
{
    solidCount = spikeCount = anchorCount = 0;

    // --- Section 1: start, simple gap ---
    AddSolid(0, 650, 520, 100, SOLID_GROUND);
    // pit: 520 - 660
    AddSolid(660, 650, 380, 100, SOLID_GROUND);

    // --- Section 2: low wall to jump over, then spikes on the ground ---
    AddSolid(1040, 460, 40, 290, SOLID_WALL);
    AddSolid(1080, 650, 320, 100, SOLID_GROUND);
    AddSpike(1180, 630, 100, 20);

    // --- Section 3: wide pit needing a grapple swing ---
    // pit: 1400 - 1650
    AddAnchor(1520, 380);
    AddSolid(1650, 650, 320, 100, SOLID_GROUND);

    // --- Section 4: floating platforms staircase (jumpable) ---
    AddSolid(2050, 540, 150, 30, SOLID_FLOATING);
    AddSolid(2260, 420, 150, 30, SOLID_FLOATING);
    AddAnchor(2340, 200); // optional grapple assist to the higher platform

    // --- Section 5: drop back down, spiky ground run ---
    AddSolid(2500, 650, 420, 100, SOLID_GROUND);
    AddSpike(2620, 630, 90, 20);
    AddSpike(2780, 630, 90, 20);

    // --- Section 6: tall wall, must swing over the top ---
    AddSolid(2960, 380, 40, 370, SOLID_WALL);
    AddAnchor(2980, 220);

    // --- Section 7: the Sky Tower ---
    // A narrow vertical chimney between two facing walls. The floor runs
    // under the whole shaft, and a stack of anchors lets the player
    // grapple-climb straight up, bouncing off either wall if they drift
    // into one too fast. Exit is a ledge poking out above the right wall.
    AddSolid(3040, 650, 540, 100, SOLID_GROUND);   // shaft floor (extends section 6's ground)
    AddSolid(3300, 250, 40, 300, SOLID_WALL);      // left shaft wall (overhang, floor below is clear to walk under)
    AddSolid(3540, 250, 40, 300, SOLID_WALL);      // right shaft wall (overhang, same)
    AddAnchor(3440, 560);
    AddAnchor(3440, 430);
    AddAnchor(3440, 300);
    AddAnchor(3440, 170);
    AddSolid(3540, 150, 220, 30, SOLID_FLOATING);  // exit ledge above the right wall

    // --- Section 8: floating staircase back down from the tower ---
    AddSolid(3760, 300, 150, 30, SOLID_FLOATING);
    AddSolid(3960, 440, 150, 30, SOLID_FLOATING);
    AddSolid(4160, 580, 150, 30, SOLID_FLOATING);
    AddSolid(4360, 650, 400, 100, SOLID_GROUND);
    AddSpike(4480, 630, 90, 20);
    AddSpike(4630, 630, 90, 20);

    // --- Section 9: final big pit + swing, then home stretch ---
    // pit: 4760 - 5060
    AddAnchor(4910, 340);
    AddSolid(5060, 650, 800, 100, SOLID_GROUND);
    AddSpike(5280, 630, 90, 20);
    AddSpike(5440, 630, 90, 20);

    // Floating bonus platform near the end (optional path)
    AddSolid(5610, 520, 160, 30, SOLID_FLOATING);
    AddAnchor(5690, 330);

    goalRect = (Rectangle){ 5760, 580, 40, 70 };

    spawnPoint = (Vector2){ 60, 650 - PLAYER_H };
}

//------------------------------------------------------------------------------------
// Collision helpers
//------------------------------------------------------------------------------------
static Rectangle PlayerRect(Vector2 pos)
{
    return (Rectangle) { pos.x, pos.y, PLAYER_W, PLAYER_H };
}

static void MoveAndCollide(Player* p, float dt)
{
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
                // Hit a wall hard enough to bounce off it instead of stopping dead.
                p->velocity.x = -p->velocity.x * WALL_BOUNCE_RESTITUTION;
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
}

// The grapple constraint repositions the player directly (it snaps them onto
// the rope circle), which bypasses MoveAndCollide's swept collision and lets
// the player's body end up inside a wall if the rope circle passes through
// one. This pushes the player back out along the shallowest overlap axis
// after any such reposition, and applies the same high-speed wall bounce
// used by normal movement.
static void ResolveSolidOverlap(Player* p)
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
            if (pushY < 0.0f) p->onGround = true; // pushed up -> was landing on top of solid
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

static void RespawnPlayer(Player* p)
{
    p->position = spawnPoint;
    p->velocity = (Vector2){ 0, 0 };
    p->grappling = false;
    p->onGround = false;
}

//------------------------------------------------------------------------------------
// Grapple
//------------------------------------------------------------------------------------
static Vector2 PlayerCenter(Player* p)
{
    return (Vector2) { p->position.x + PLAYER_W * 0.5f, p->position.y + PLAYER_H * 0.5f };
}

// Casts a ray from origin along the unit vector dir and returns the distance
// to the nearest solid it hits, capped at maxDist (returned if nothing is
// hit). Uses the standard slab method for ray-vs-AABB intersection. This is
// what stops the grapple rope from reeling the player straight through a
// wall that sits between the anchor and them: no matter how much the rope
// wants to shorten, it can't get shorter than the distance to that wall.
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

        // Small epsilon so a solid the ray starts exactly touching doesn't
        // clamp the rope to ~0.
        if (tmin > 0.0001f && tmin < nearest) nearest = tmin;
    }
    return nearest;
}

static void TryFireGrapple(Player* p)
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
    }
}

static void UpdateGrapple(Player* p, float dt)
{
    if (!p->grappling) return;

    // Reel in / out
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

        // The rope can't reel any shorter than the distance to whatever
        // solid stands between the anchor and the player -- otherwise
        // holding reel-in (or even just a taut swing) would keep recomputing
        // a target point past the obstruction, walking the player through
        // it a few pixels a frame even though ResolveSolidOverlap pushes
        // them back out afterward each time.
        float wallDist = RaycastSolids(p->grappleAnchor, dir, dist);
        if (wallDist < p->ropeLength) p->ropeLength = wallDist;

        // Snap back onto the rope circle
        Vector2 newCenter = Vector2Add(p->grappleAnchor, Vector2Scale(dir, p->ropeLength));
        p->position.x = newCenter.x - PLAYER_W * 0.5f;
        p->position.y = newCenter.y - PLAYER_H * 0.5f;

        // The raycast above is a single point and doesn't know the player's
        // width/height, so it can still leave the body edge-on into a wall;
        // push back out and apply wall-bounce as a final cleanup.
        ResolveSolidOverlap(p);

        // Remove outward radial velocity (rope is taut, not a spring)
        float radialSpeed = Vector2DotProduct(p->velocity, dir);
        if (radialSpeed > 0.0f)
        {
            p->velocity = Vector2Subtract(p->velocity, Vector2Scale(dir, radialSpeed));
        }

        // Small tangential assist so swings feel snappy
        Vector2 tangent = (Vector2){ -dir.y, dir.x };
        float tangentSpeed = Vector2DotProduct(p->velocity, tangent);
        float sign = (tangentSpeed >= 0) ? 1.0f : -1.0f;
        p->velocity = Vector2Add(p->velocity, Vector2Scale(tangent, sign * GRAPPLE_PULL_ACC * dt * 0.15f));
    }
}

//------------------------------------------------------------------------------------
// Drawing
//------------------------------------------------------------------------------------
static void DrawSolid(Solid* s)
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
        // grassy top strip
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

static void DrawAnchor(Vector2 a, bool inRange)
{
    DrawCircleV(a, 9, inRange ? (Color) { 250, 210, 60, 255 } : (Color) { 90, 110, 170, 255 });
    DrawCircleLines((int)a.x, (int)a.y, 9, (Color) { 20, 20, 30, 255 });
    DrawCircleLines((int)a.x, (int)a.y, (int)GRAPPLE_RANGE, (Color) { 90, 110, 170, 40 });
}

static void DrawPlayer(Player* p)
{
    Rectangle pr = PlayerRect(p->position);
    DrawRectangleRec(pr, (Color) { 200, 60, 60, 255 });
    DrawRectangleLinesEx(pr, 2, (Color) { 60, 10, 10, 255 });
    // eye to show facing
    float eyeX = pr.x + PLAYER_W * 0.5f + p->facing * 8.0f;
    DrawCircle((int)eyeX, (int)(pr.y + 14), 3, WHITE);
}

//------------------------------------------------------------------------------------
// Main
//------------------------------------------------------------------------------------
int main(void)
{
    InitWindow(SCREEN_W, SCREEN_H, "AI Platformer - Momentum & Grapple");
    SetTargetFPS(60);

    BuildLevel();

    Player player = { 0 };
    RespawnPlayer(&player);
    player.facing = 1.0f;

    Camera2D camera = { 0 };
    camera.offset = (Vector2){ SCREEN_W / 2.0f, SCREEN_H / 2.0f };
    camera.zoom = 1.0f;

    bool won = false;

    while (!WindowShouldClose())
    {
        float dt = GetFrameTime();
        if (dt > 1.0f / 30.0f) dt = 1.0f / 30.0f; // clamp huge frame spikes

        if (IsKeyPressed(KEY_R))
        {
            RespawnPlayer(&player);
            won = false;
        }

        if (!won)
        {
            // ---- Horizontal input / momentum ----
            float moveDir = 0.0f;
            if (IsKeyDown(KEY_RIGHT) || IsKeyDown(KEY_D)) moveDir += 1.0f;
            if (IsKeyDown(KEY_LEFT) || IsKeyDown(KEY_A)) moveDir -= 1.0f;
            if (moveDir != 0.0f) player.facing = moveDir;

            float accel = player.onGround ? GROUND_ACCEL : AIR_ACCEL;
            float friction = player.onGround ? GROUND_FRICTION : AIR_FRICTION;

            if (moveDir != 0.0f)
            {
                player.velocity.x += moveDir * accel * dt;
                // Only clamp when input is actively pushing past the cap;
                // momentum from swings/falls can still exceed this.
                if (!player.grappling)
                {
                    if (player.velocity.x > MAX_RUN_SPEED) player.velocity.x = MAX_RUN_SPEED;
                    if (player.velocity.x < -MAX_RUN_SPEED) player.velocity.x = -MAX_RUN_SPEED;
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

            // ---- Jump ----
            if (IsKeyPressed(KEY_SPACE) && player.onGround)
            {
                player.velocity.y = JUMP_VELOCITY;
                player.onGround = false;
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

            // ---- Hazards ----
            if (TouchesAnySpike(&player)) RespawnPlayer(&player);
            if (player.position.y > WORLD_DEATH_Y) RespawnPlayer(&player);

            // ---- Goal ----
            if (CheckCollisionRecs(PlayerRect(player.position), goalRect)) won = true;
        }

        // ---- Camera ----
        camera.target = PlayerCenter(&player);
        float halfW = SCREEN_W / 2.0f;
        float halfH = SCREEN_H / 2.0f;
        if (camera.target.x < halfW) camera.target.x = halfW;
        if (camera.target.x > worldWidth - halfW) camera.target.x = worldWidth - halfW;
        if (camera.target.y < worldTopY + halfH) camera.target.y = worldTopY + halfH;
        if (camera.target.y > worldBottomY - halfH) camera.target.y = worldBottomY - halfH;

        // ---- Draw ----
        BeginDrawing();
        ClearBackground((Color) { 190, 220, 245, 255 });

        BeginMode2D(camera);

        for (int i = 0; i < solidCount; i++) DrawSolid(&solids[i]);
        for (int i = 0; i < spikeCount; i++) DrawSpike(spikes[i]);

        Vector2 pc = PlayerCenter(&player);
        for (int i = 0; i < anchorCount; i++)
        {
            bool inRange = Vector2Distance(pc, anchors[i]) <= GRAPPLE_RANGE;
            DrawAnchor(anchors[i], inRange);
        }

        // goal flag
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

        DrawPlayer(&player);

        EndMode2D();

        // ---- HUD ----
        DrawRectangle(0, 0, SCREEN_W, 84, Fade(BLACK, 0.35f));
        DrawText("A/D or Arrows: run   SPACE: jump   F / Left-Click: grapple nearest anchor",
            16, 8, 18, RAYWHITE);
        DrawText("While grappling -> W/S or Up/Down: reel in/out    R: restart level",
            16, 32, 18, RAYWHITE);
        DrawText("Hit a wall hard enough and you'll bounce off it",
            16, 56, 16, (Color) { 220, 220, 220, 255 });

        if (won)
        {
            const char* msg = "LEVEL COMPLETE! Press R to play again.";
            int w = MeasureText(msg, 40);
            DrawRectangle(SCREEN_W / 2 - w / 2 - 20, SCREEN_H / 2 - 40, w + 40, 80, Fade(BLACK, 0.6f));
            DrawText(msg, SCREEN_W / 2 - w / 2, SCREEN_H / 2 - 20, 40, (Color) { 250, 220, 80, 255 });
        }

        DrawFPS(SCREEN_W - 90, SCREEN_H - 24);

        EndDrawing();
    }

    CloseWindow();
    return 0;
}