#include "raylib.h"

int main(void)
{
    const int screenWidth = 1280;
    const int screenHeight = 720;

    InitWindow(screenWidth, screenHeight, "AI Platformer");
    SetTargetFPS(60);

    Vector2 playerPos = { screenWidth / 2.0f, screenHeight / 2.0f };
    const float playerSpeed = 300.0f;

    while (!WindowShouldClose())
    {
        float dt = GetFrameTime();

        if (IsKeyDown(KEY_RIGHT)) playerPos.x += playerSpeed * dt;
        if (IsKeyDown(KEY_LEFT))  playerPos.x -= playerSpeed * dt;
        if (IsKeyDown(KEY_UP))    playerPos.y -= playerSpeed * dt;
        if (IsKeyDown(KEY_DOWN))  playerPos.y += playerSpeed * dt;

        BeginDrawing();
        ClearBackground(RAYWHITE);

        DrawRectangle((int)playerPos.x - 20, (int)playerPos.y - 20, 40, 40, MAROON);
        DrawText("AI Platformer - raylib template", 20, 20, 20, DARKGRAY);
        DrawFPS(screenWidth - 100, 20);

        EndDrawing();
    }

    CloseWindow();

    return 0;
}
