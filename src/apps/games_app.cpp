#include "app_api.h"
#include "display.h"
#include "settings.h"
#include "config.h"
#include "audio.h"
#include "menu.h"

namespace GamesApp {

enum Mode {
    MODE_MENU = 0,
    MODE_SNAKE,
    MODE_PONG
};

static Mode currentMode = MODE_MENU;
static Menu *subSubMenu = nullptr;
static bool exitApp = false;

// Snake Game
struct Point { int16_t x, y; };
static Point snake[100];
static int snakeLen = 3;
static Point food;
static int dirX = 1, dirY = 0;
static int score = 0;
static bool snakeGameOver = false;
static uint32_t lastSnakeMs = 0;

static void resetSnake() {
    snakeLen = 3;
    snake[0] = { 10, 8 };
    snake[1] = { 9, 8 };
    snake[2] = { 8, 8 };
    dirX = 1; dirY = 0;
    score = 0;
    snakeGameOver = false;
    food = { (int16_t)(random(2, 24)), (int16_t)(random(2, 16)) };
}

static void stepSnake() {
    if (snakeGameOver) return;

    Point head = { (int16_t)(snake[0].x + dirX), (int16_t)(snake[0].y + dirY) };
    if (head.x < 0 || head.x >= 26 || head.y < 0 || head.y >= 18) {
        snakeGameOver = true;
        return;
    }
    for (int i = 0; i < snakeLen; i++) {
        if (snake[i].x == head.x && snake[i].y == head.y) {
            snakeGameOver = true;
            return;
        }
    }

    for (int i = snakeLen; i > 0; i--) snake[i] = snake[i - 1];
    snake[0] = head;

    if (head.x == food.x && head.y == food.y) {
        if (snakeLen < 99) snakeLen++;
        score += 10;
        audioClickOk();
        food = { (int16_t)(random(1, 25)), (int16_t)(random(1, 17)) };
    }
}

static void drawSnake() {
    const Theme &t = gSettings.theme();
    gfx->fillRect(0, TOPBAR_HEIGHT, SCREEN_W, SCREEN_H - TOPBAR_HEIGHT, t.bg);

    int gridX = 4, gridY = TOPBAR_HEIGHT + 4, sz = 12;
    gfx->drawRect(gridX - 1, gridY - 1, 26 * sz + 2, 18 * sz + 2, t.dim);

    // Draw Food
    gfx->fillRect(gridX + food.x * sz, gridY + food.y * sz, sz, sz, t.warn);

    // Draw Snake
    for (int i = 0; i < snakeLen; i++) {
        gfx->fillRect(gridX + snake[i].x * sz, gridY + snake[i].y * sz, sz, sz, (i == 0) ? t.accent : t.fg);
    }

    gfx->setTextSize(1);
    gfx->setTextColor(t.fg);
    gfx->setCursor(8, SCREEN_H - 12);
    gfx->print("Score: " + String(score));

    if (snakeGameOver) {
        gfx->setTextColor(t.bad);
        gfx->setTextSize(2);
        gfx->setCursor(80, 110);
        gfx->print("GAME OVER!");
        gfx->setTextSize(1);
        gfx->setCursor(75, 135);
        gfx->print("Press OK to restart");
    }
}

// --- Pong Game ---
static int paddleY = 80;
static int aiPaddleY = 80;
static float ballX = 160, ballY = 120;
static float ballVx = 2.5f, ballVy = 1.8f;
static int p1Score = 0, p2Score = 0;
static uint32_t lastPongMs = 0;

static void resetPong() {
    paddleY = 80;
    aiPaddleY = 80;
    ballX = 160; ballY = 120;
    ballVx = 2.5f; ballVy = 1.8f;
    p1Score = 0; p2Score = 0;
}

static void stepPong() {
    ballX += ballVx;
    ballY += ballVy;

    int minY = TOPBAR_HEIGHT + 4, maxY = SCREEN_H - 10;
    if (ballY <= minY || ballY >= maxY) ballVy = -ballVy;

    // Computer movement
    if (aiPaddleY + 15 < ballY) aiPaddleY += 2;
    else if (aiPaddleY + 15 > ballY) aiPaddleY -= 2;

    // Player collision
    if (ballX <= 16 && ballY >= paddleY && ballY <= paddleY + 30) {
        ballVx = -ballVx;
        ballX = 17;
        audioClickNav();
    }
    // Computer collision
    if (ballX >= SCREEN_W - 16 && ballY >= aiPaddleY && ballY <= aiPaddleY + 30) {
        ballVx = -ballVx;
        ballX = SCREEN_W - 17;
    }

    // Scoring
    if (ballX < 0) {
        p2Score++;
        ballX = 160; ballY = 120; ballVx = 2.5f;
    } else if (ballX > SCREEN_W) {
        p1Score++;
        ballX = 160; ballY = 120; ballVx = -2.5f;
    }
}

static void drawPong() {
    const Theme &t = gSettings.theme();
    gfx->fillRect(0, TOPBAR_HEIGHT, SCREEN_W, SCREEN_H - TOPBAR_HEIGHT, t.bg);

    // Paddles
    gfx->fillRect(8, paddleY, 6, 30, t.accent);
    gfx->fillRect(SCREEN_W - 14, aiPaddleY, 6, 30, t.warn);

    // Ball
    gfx->fillCircle((int)ballX, (int)ballY, 3, t.fg);

    // Scores
    gfx->setTextSize(1);
    gfx->setTextColor(t.fg);
    gfx->setCursor(80, TOPBAR_HEIGHT + 10);
    gfx->print("P1: " + String(p1Score) + "   CPU: " + String(p2Score));
}

static void init() {
    exitApp = false;
    currentMode = MODE_MENU;

    if (subSubMenu) delete subSubMenu;
    std::vector<MenuItem> items = {
        { "Snake",  ">", [](){ currentMode = MODE_SNAKE; resetSnake(); drawSnake(); } },
        { "Pong",   ">", [](){ currentMode = MODE_PONG; resetPong(); drawPong(); } }
    };
    subSubMenu = new Menu("Arcade Games", items);
    subSubMenu->draw();
}

static void tick() {
    if (currentMode == MODE_MENU && subSubMenu) {
        subSubMenu->tick();
        return;
    }

    if (currentMode == MODE_SNAKE) {
        if (millis() - lastSnakeMs > 140) {
            lastSnakeMs = millis();
            stepSnake();
            drawSnake();
        }
    } else if (currentMode == MODE_PONG) {
        if (millis() - lastPongMs > 30) {
            lastPongMs = millis();
            stepPong();
            drawPong();
        }
    }
}

static void handleInput(const InputResult &in) {
    if (currentMode == MODE_MENU) {
        if (in.type == InputEvent::BACK) {
            exitApp = true;
            audioClickBack();
            return;
        }
        if (subSubMenu) subSubMenu->handleInput(in);
        return;
    }

    if (in.type == InputEvent::BACK) {
        currentMode = MODE_MENU;
        audioClickBack();
        if (subSubMenu) subSubMenu->draw();
        return;
    }

    if (currentMode == MODE_SNAKE) {
        if (in.type == InputEvent::NAV_UP && dirY == 0)    { dirX = 0; dirY = -1; }
        if (in.type == InputEvent::NAV_DOWN && dirY == 0)  { dirX = 0; dirY = 1; }
        if (in.type == InputEvent::NAV_LEFT && dirX == 0)  { dirX = -1; dirY = 0; }
        if (in.type == InputEvent::NAV_RIGHT && dirX == 0) { dirX = 1; dirY = 0; }
        if (snakeGameOver && in.type == InputEvent::OK)     { resetSnake(); drawSnake(); }
    } else if (currentMode == MODE_PONG) {
        if (in.type == InputEvent::NAV_UP && paddleY > TOPBAR_HEIGHT + 4) paddleY -= 8;
        if (in.type == InputEvent::NAV_DOWN && paddleY < SCREEN_H - 34) paddleY += 8;
    }
}

static void onExit() {
    if (subSubMenu) {
        delete subSubMenu;
        subSubMenu = nullptr;
    }
}

static bool wantsExit() { return exitApp; }

} 

AppModule gamesAppGet() {
    return { GamesApp::init, GamesApp::tick, GamesApp::handleInput, GamesApp::onExit, GamesApp::wantsExit };
}
