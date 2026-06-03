#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>

#ifdef _MSC_VER
#pragma comment(lib, "ws2_32.lib")
#endif

namespace {

const int kWidth = 1120;
const int kHeight = 720;
const int kPort = 54000;
const float kDt = 1.0f / 60.0f;
const float kArenaLeft = 40.0f;
const float kArenaTop = 76.0f;
const float kArenaRight = 1080.0f;
const float kArenaBottom = 670.0f;
const uint32_t kMagic = 0x59475950; // PYGY
const uint32_t kVersion = 1;

template <typename T>
T clampValue(T value, T lo, T hi) {
    return value < lo ? lo : (value > hi ? hi : value);
}

float length2(float x, float y) {
    return std::sqrt(x * x + y * y);
}

float distance2(float ax, float ay, float bx, float by) {
    return length2(ax - bx, ay - by);
}

struct InputFrame {
    uint32_t magic;
    uint32_t version;
    uint32_t sequence;
    uint32_t buttons;
    float aimX;
    float aimY;
};

enum ButtonBits {
    BtnUp = 1 << 0,
    BtnDown = 1 << 1,
    BtnLeft = 1 << 2,
    BtnRight = 1 << 3,
    BtnAttack = 1 << 4,
    BtnSkill = 1 << 5,
    BtnUtility = 1 << 6,
    BtnStart = 1 << 7,
    BtnReset = 1 << 8
};

struct PlayerState {
    float x;
    float y;
    float vx;
    float vy;
    float hp;
    float attackCd;
    float skillCd;
    float utilityCd;
    int score;
    int kills;
    int role;
    uint32_t flags;
};

struct ProjectileState {
    float x;
    float y;
    float vx;
    float vy;
    float life;
    int owner;
    int kind;
    int active;
};

struct LightState {
    float x;
    float y;
    float radius;
    float energy;
    int active;
};

struct WorldSnapshot {
    uint32_t magic;
    uint32_t version;
    uint32_t sequence;
    uint32_t phase;
    PlayerState players[2];
    ProjectileState projectiles[32];
    LightState lights[3];
    float capture;
    float matchTime;
    int winner;
    int connected;
    int latencyMs;
};

struct NetShared {
    std::mutex mutex;
    bool running = false;
    bool connected = false;
    bool clientInputReady = false;
    int latencyMs = 0;
    SOCKET listenSocket = INVALID_SOCKET;
    SOCKET socket = INVALID_SOCKET;
    InputFrame clientInput{};
    WorldSnapshot snapshot{};
};

enum AppMode {
    ModeMenu,
    ModeHost,
    ModeClient,
    ModeOffline
};

enum GamePhase {
    PhaseWaiting,
    PhasePlaying,
    PhaseGameOver
};

class Game {
public:
    Game() {
        reset();
    }

    void reset() {
        std::memset(&world, 0, sizeof(world));
        world.magic = kMagic;
        world.version = kVersion;
        world.phase = PhaseWaiting;
        world.matchTime = 0.0f;
        world.capture = 0.0f;
        world.winner = -1;
        setupPlayer(0, 190.0f, 360.0f, 0);
        setupPlayer(1, 930.0f, 360.0f, 1);
        world.lights[0].x = 560.0f;
        world.lights[0].y = 360.0f;
        world.lights[0].radius = 145.0f;
        world.lights[0].energy = 1.0f;
        world.lights[0].active = 1;
        world.lights[1].x = 280.0f;
        world.lights[1].y = 190.0f;
        world.lights[1].radius = 100.0f;
        world.lights[1].energy = 0.7f;
        world.lights[1].active = 1;
        world.lights[2].x = 820.0f;
        world.lights[2].y = 520.0f;
        world.lights[2].radius = 100.0f;
        world.lights[2].energy = 0.7f;
        world.lights[2].active = 1;
        localInputs[0] = makeEmptyInput();
        localInputs[1] = makeEmptyInput();
    }

    InputFrame makeEmptyInput() const {
        InputFrame input{};
        input.magic = kMagic;
        input.version = kVersion;
        input.aimX = 1.0f;
        input.aimY = 0.0f;
        return input;
    }

    void setInput(int index, const InputFrame& input) {
        if (index >= 0 && index < 2) {
            localInputs[index] = input;
        }
    }

    void startIfReady(bool connectedOrOffline) {
        if (world.phase == PhaseWaiting && connectedOrOffline) {
            world.phase = PhasePlaying;
        }
    }

    void update(float dt, bool connectedOrOffline) {
        if (localInputs[0].buttons & BtnReset || localInputs[1].buttons & BtnReset) {
            reset();
            startIfReady(connectedOrOffline);
            return;
        }
        startIfReady(connectedOrOffline);
        if (world.phase != PhasePlaying) {
            world.sequence++;
            return;
        }

        world.matchTime += dt;
        updatePlayer(0, localInputs[0], dt);
        updatePlayer(1, localInputs[1], dt);
        updateProjectiles(dt);
        updateCapture(dt);
        updateLights(dt);
        checkWin();
        world.sequence++;
    }

    const WorldSnapshot& snapshot() const {
        return world;
    }

    void loadSnapshot(const WorldSnapshot& incoming) {
        world = incoming;
    }

private:
    WorldSnapshot world{};
    InputFrame localInputs[2]{};

    void setupPlayer(int index, float x, float y, int role) {
        PlayerState& p = world.players[index];
        p.x = x;
        p.y = y;
        p.vx = 0.0f;
        p.vy = 0.0f;
        p.hp = 100.0f;
        p.attackCd = 0.0f;
        p.skillCd = 0.0f;
        p.utilityCd = 0.0f;
        p.score = 0;
        p.kills = 0;
        p.role = role;
        p.flags = 0;
    }

    bool inLight(float x, float y) const {
        for (int i = 0; i < 3; ++i) {
            if (world.lights[i].active && distance2(x, y, world.lights[i].x, world.lights[i].y) < world.lights[i].radius) {
                return true;
            }
        }
        return false;
    }

    void updatePlayer(int index, const InputFrame& input, float dt) {
        PlayerState& p = world.players[index];
        float dx = 0.0f;
        float dy = 0.0f;
        if (input.buttons & BtnLeft) dx -= 1.0f;
        if (input.buttons & BtnRight) dx += 1.0f;
        if (input.buttons & BtnUp) dy -= 1.0f;
        if (input.buttons & BtnDown) dy += 1.0f;
        float len = length2(dx, dy);
        if (len > 0.01f) {
            dx /= len;
            dy /= len;
        }

        bool lit = inLight(p.x, p.y);
        float speed = p.role == 0 ? 220.0f : 235.0f;
        if (p.role == 0 && lit) speed += 35.0f;
        if (p.role == 1 && !lit) speed += 45.0f;
        p.vx = dx * speed;
        p.vy = dy * speed;
        p.x = clampValue(p.x + p.vx * dt, kArenaLeft + 20.0f, kArenaRight - 20.0f);
        p.y = clampValue(p.y + p.vy * dt, kArenaTop + 20.0f, kArenaBottom - 20.0f);
        p.attackCd = clampValue(p.attackCd - dt, 0.0f, 99.0f);
        p.skillCd = clampValue(p.skillCd - dt, 0.0f, 99.0f);
        p.utilityCd = clampValue(p.utilityCd - dt, 0.0f, 99.0f);
        p.flags = lit ? 1u : 0u;

        float ax = input.aimX;
        float ay = input.aimY;
        float alen = length2(ax, ay);
        if (alen < 0.01f) {
            ax = index == 0 ? 1.0f : -1.0f;
            ay = 0.0f;
        } else {
            ax /= alen;
            ay /= alen;
        }

        if ((input.buttons & BtnAttack) && p.attackCd <= 0.0f) {
            spawnProjectile(index, p.x + ax * 26.0f, p.y + ay * 26.0f, ax * 580.0f, ay * 580.0f, p.role == 0 ? 0 : 1);
            p.attackCd = p.role == 0 ? 0.45f : 0.55f;
        }
        if ((input.buttons & BtnSkill) && p.skillCd <= 0.0f) {
            if (p.role == 0) {
                flashLight(index, p.x, p.y, 130.0f);
            } else {
                shadowDash(index, ax, ay);
            }
            p.skillCd = p.role == 0 ? 4.0f : 3.2f;
        }
        if ((input.buttons & BtnUtility) && p.utilityCd <= 0.0f) {
            if (p.role == 0) {
                placeLight(p.x, p.y);
            } else {
                castShadowField(index, p.x, p.y);
            }
            p.utilityCd = 5.0f;
        }
    }

    void spawnProjectile(int owner, float x, float y, float vx, float vy, int kind) {
        for (int i = 0; i < 32; ++i) {
            ProjectileState& b = world.projectiles[i];
            if (!b.active) {
                b.x = x;
                b.y = y;
                b.vx = vx;
                b.vy = vy;
                b.life = kind == 2 ? 0.65f : 1.8f;
                b.owner = owner;
                b.kind = kind;
                b.active = 1;
                return;
            }
        }
    }

    void flashLight(int owner, float x, float y, float radius) {
        spawnProjectile(owner, x, y, 0.0f, 0.0f, 2);
        int target = owner == 0 ? 1 : 0;
        if (distance2(x, y, world.players[target].x, world.players[target].y) < radius) {
            damagePlayer(owner, target, 18.0f);
        }
    }

    void shadowDash(int index, float ax, float ay) {
        PlayerState& p = world.players[index];
        p.x = clampValue(p.x + ax * 118.0f, kArenaLeft + 20.0f, kArenaRight - 20.0f);
        p.y = clampValue(p.y + ay * 118.0f, kArenaTop + 20.0f, kArenaBottom - 20.0f);
        int target = index == 0 ? 1 : 0;
        if (distance2(p.x, p.y, world.players[target].x, world.players[target].y) < 72.0f) {
            damagePlayer(index, target, inLight(p.x, p.y) ? 12.0f : 26.0f);
        }
    }

    void castShadowField(int owner, float x, float y) {
        spawnProjectile(owner, x, y, 0.0f, 0.0f, 3);
        for (int i = 0; i < 3; ++i) {
            if (world.lights[i].active && distance2(x, y, world.lights[i].x, world.lights[i].y) < world.lights[i].radius + 55.0f) {
                world.lights[i].energy = clampValue(world.lights[i].energy - 0.28f, 0.25f, 1.0f);
            }
        }
    }

    void placeLight(float x, float y) {
        int slot = 1;
        if (world.lights[1].energy > world.lights[2].energy) slot = 2;
        world.lights[slot].x = x;
        world.lights[slot].y = y;
        world.lights[slot].radius = 112.0f;
        world.lights[slot].energy = 1.0f;
        world.lights[slot].active = 1;
    }

    void updateProjectiles(float dt) {
        for (int i = 0; i < 32; ++i) {
            ProjectileState& b = world.projectiles[i];
            if (!b.active) continue;
            b.life -= dt;
            b.x += b.vx * dt;
            b.y += b.vy * dt;
            if (b.x < kArenaLeft || b.x > kArenaRight || b.y < kArenaTop || b.y > kArenaBottom || b.life <= 0.0f) {
                b.active = 0;
                continue;
            }
            if (b.kind == 0 || b.kind == 1) {
                int target = b.owner == 0 ? 1 : 0;
                if (distance2(b.x, b.y, world.players[target].x, world.players[target].y) < 26.0f) {
                    float base = b.kind == 0 ? 18.0f : 16.0f;
                    if (b.kind == 0 && inLight(world.players[b.owner].x, world.players[b.owner].y)) base += 7.0f;
                    if (b.kind == 1 && !inLight(world.players[b.owner].x, world.players[b.owner].y)) base += 8.0f;
                    damagePlayer(b.owner, target, base);
                    b.active = 0;
                }
            }
        }
    }

    void damagePlayer(int attacker, int target, float amount) {
        PlayerState& victim = world.players[target];
        victim.hp -= amount;
        if (victim.hp <= 0.0f) {
            world.players[attacker].kills += 1;
            world.players[attacker].score += 1;
            setupPlayer(target, target == 0 ? 190.0f : 930.0f, 360.0f, target);
        }
    }

    void updateCapture(float dt) {
        float centerX = 560.0f;
        float centerY = 360.0f;
        bool lightOnPoint = distance2(world.players[0].x, world.players[0].y, centerX, centerY) < 105.0f;
        bool shadowOnPoint = distance2(world.players[1].x, world.players[1].y, centerX, centerY) < 105.0f;
        if (lightOnPoint && !shadowOnPoint) {
            world.capture += dt * 10.0f;
        } else if (shadowOnPoint && !lightOnPoint) {
            world.capture -= dt * 10.0f;
        }
        world.capture = clampValue(world.capture, -100.0f, 100.0f);
        if (world.capture >= 100.0f) {
            world.players[0].score += 1;
            world.capture = 0.0f;
        }
        if (world.capture <= -100.0f) {
            world.players[1].score += 1;
            world.capture = 0.0f;
        }
    }

    void updateLights(float dt) {
        for (int i = 0; i < 3; ++i) {
            if (!world.lights[i].active) continue;
            if (i > 0) {
                world.lights[i].energy -= dt * 0.035f;
            } else {
                world.lights[i].energy = 1.0f;
            }
            world.lights[i].energy = clampValue(world.lights[i].energy, 0.0f, 1.0f);
            world.lights[i].radius = (i == 0 ? 145.0f : 112.0f) * (0.45f + world.lights[i].energy * 0.55f);
            if (i > 0 && world.lights[i].energy <= 0.01f) {
                world.lights[i].active = 0;
            }
        }
    }

    void checkWin() {
        if (world.players[0].score >= 3) {
            world.winner = 0;
            world.phase = PhaseGameOver;
        }
        if (world.players[1].score >= 3) {
            world.winner = 1;
            world.phase = PhaseGameOver;
        }
    }
};

class App {
public:
    App() {
        std::strcpy(joinIp, "127.0.0.1");
        localInput = game.makeEmptyInput();
        remoteInput = game.makeEmptyInput();
    }

    ~App() {
        stopNetwork();
    }

    void onKeyDown(WPARAM key) {
        keys[key & 0xff] = true;
        if (mode == ModeMenu) {
            if (showGuide) {
                if (key == VK_ESCAPE || key == 'B' || key == VK_BACK) {
                    showGuide = false;
                }
                return;
            }
            if (key == 'H') startHost();
            else if (key == 'J') startClient();
            else if (key == 'O') startOffline();
            else if (key == VK_BACK) {
                size_t len = std::strlen(joinIp);
                if (len > 0) joinIp[len - 1] = 0;
            } else if ((key >= '0' && key <= '9') || key == VK_OEM_PERIOD) {
                size_t len = std::strlen(joinIp);
                if (len < sizeof(joinIp) - 1) {
                    joinIp[len] = key == VK_OEM_PERIOD ? '.' : static_cast<char>(key);
                    joinIp[len + 1] = 0;
                }
            }
        }
    }

    void onKeyUp(WPARAM key) {
        keys[key & 0xff] = false;
    }

    void onMouseDown(int x, int y) {
        if (mode != ModeMenu) return;
        if (showGuide) {
            if (isInside(x, y, 52, 622, 220, 664)) {
                showGuide = false;
            }
            return;
        }
        if (isInside(x, y, 88, 386, 318, 430)) {
            showGuide = true;
        }
    }

    void tick() {
        if (mode == ModeMenu) return;
        if (mode == ModeOffline) {
            InputFrame a = readInputForLight();
            InputFrame b = readInputForShadowLocal();
            game.setInput(0, a);
            game.setInput(1, b);
            game.update(kDt, true);
            visibleWorld = game.snapshot();
            return;
        }

        if (mode == ModeHost) {
            InputFrame a = readInputForLight();
            InputFrame b = game.makeEmptyInput();
            bool connected = false;
            int latency = 0;
            {
                std::lock_guard<std::mutex> lock(net.mutex);
                b = net.clientInputReady ? net.clientInput : game.makeEmptyInput();
                connected = net.connected;
                latency = net.latencyMs;
            }
            game.setInput(0, a);
            game.setInput(1, b);
            game.update(kDt, connected);
            visibleWorld = game.snapshot();
            visibleWorld.connected = connected ? 1 : 0;
            visibleWorld.latencyMs = latency;
            {
                std::lock_guard<std::mutex> lock(net.mutex);
                net.snapshot = visibleWorld;
            }
            return;
        }

        if (mode == ModeClient) {
            localInput = readInputForClientShadow();
            sendClientInput(localInput);
            WorldSnapshot snap{};
            bool hasSnap = false;
            {
                std::lock_guard<std::mutex> lock(net.mutex);
                if (net.snapshot.magic == kMagic) {
                    snap = net.snapshot;
                    hasSnap = true;
                }
            }
            if (hasSnap) {
                visibleWorld = snap;
            }
        }
    }

    void draw(HDC hdc) {
        RECT rc{0, 0, kWidth, kHeight};
        HBRUSH bg = CreateSolidBrush(RGB(14, 17, 28));
        FillRect(hdc, &rc, bg);
        DeleteObject(bg);
        if (mode == ModeMenu) {
            if (showGuide) drawGuide(hdc);
            else drawMenu(hdc);
        } else {
            drawWorld(hdc, visibleWorld);
        }
    }

private:
    AppMode mode = ModeMenu;
    bool showGuide = false;
    bool keys[256]{};
    char joinIp[64]{};
    Game game;
    WorldSnapshot visibleWorld{};
    InputFrame localInput{};
    InputFrame remoteInput{};
    NetShared net;
    std::thread netThread;
    uint32_t inputSequence = 0;

    bool isInside(int x, int y, int l, int t, int r, int b) const {
        return x >= l && x <= r && y >= t && y <= b;
    }

    void startHost() {
        stopNetwork();
        mode = ModeHost;
        game.reset();
        visibleWorld = game.snapshot();
        net.running = true;
        netThread = std::thread([this]() { hostThread(); });
    }

    void startClient() {
        stopNetwork();
        mode = ModeClient;
        game.reset();
        visibleWorld = game.snapshot();
        net.running = true;
        netThread = std::thread([this]() { clientThread(); });
    }

    void startOffline() {
        stopNetwork();
        mode = ModeOffline;
        game.reset();
        visibleWorld = game.snapshot();
    }

    void stopNetwork() {
        net.running = false;
        {
            std::lock_guard<std::mutex> lock(net.mutex);
            if (net.socket != INVALID_SOCKET) {
                closesocket(net.socket);
                net.socket = INVALID_SOCKET;
            }
            if (net.listenSocket != INVALID_SOCKET) {
                closesocket(net.listenSocket);
                net.listenSocket = INVALID_SOCKET;
            }
            net.connected = false;
        }
        if (netThread.joinable()) {
            netThread.join();
        }
        WSACleanup();
    }

    InputFrame readInputForLight() {
        InputFrame input = game.makeEmptyInput();
        input.sequence = ++inputSequence;
        if (keys['W']) input.buttons |= BtnUp;
        if (keys['S']) input.buttons |= BtnDown;
        if (keys['A']) input.buttons |= BtnLeft;
        if (keys['D']) input.buttons |= BtnRight;
        if (keys['F']) input.buttons |= BtnAttack;
        if (keys['G']) input.buttons |= BtnSkill;
        if (keys['Q']) input.buttons |= BtnUtility;
        if (keys['R']) input.buttons |= BtnReset;
        input.aimX = 1.0f;
        input.aimY = 0.0f;
        if (keys['A']) input.aimX = -1.0f;
        if (keys['D']) input.aimX = 1.0f;
        if (keys['W']) input.aimY = -0.3f;
        if (keys['S']) input.aimY = 0.3f;
        return input;
    }

    InputFrame readInputForClientShadow() {
        InputFrame input = readInputForLight();
        input.aimX = -input.aimX;
        return input;
    }

    InputFrame readInputForShadowLocal() {
        InputFrame input = game.makeEmptyInput();
        input.sequence = ++inputSequence;
        if (keys[VK_UP]) input.buttons |= BtnUp;
        if (keys[VK_DOWN]) input.buttons |= BtnDown;
        if (keys[VK_LEFT]) input.buttons |= BtnLeft;
        if (keys[VK_RIGHT]) input.buttons |= BtnRight;
        if (keys[VK_RETURN]) input.buttons |= BtnAttack;
        if (keys['K'] || keys[VK_SHIFT] || keys[VK_RSHIFT]) input.buttons |= BtnSkill;
        if (keys['L'] || keys[VK_CONTROL] || keys[VK_RCONTROL]) input.buttons |= BtnUtility;
        if (keys['R']) input.buttons |= BtnReset;
        input.aimX = -1.0f;
        input.aimY = 0.0f;
        if (keys[VK_LEFT]) input.aimX = -1.0f;
        if (keys[VK_RIGHT]) input.aimX = 1.0f;
        if (keys[VK_UP]) input.aimY = -0.3f;
        if (keys[VK_DOWN]) input.aimY = 0.3f;
        return input;
    }

    void hostThread() {
        WSADATA data{};
        if (WSAStartup(MAKEWORD(2, 2), &data) != 0) return;
        SOCKET listenSock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (listenSock == INVALID_SOCKET) return;
        {
            std::lock_guard<std::mutex> lock(net.mutex);
            net.listenSocket = listenSock;
        }
        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_port = htons(kPort);
        addr.sin_addr.s_addr = htonl(INADDR_ANY);
        int yes = 1;
        setsockopt(listenSock, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&yes), sizeof(yes));
        if (bind(listenSock, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == SOCKET_ERROR) return;
        if (listen(listenSock, 1) == SOCKET_ERROR) return;
        u_long nonBlocking = 1;
        ioctlsocket(listenSock, FIONBIO, &nonBlocking);

        SOCKET client = INVALID_SOCKET;
        while (net.running && client == INVALID_SOCKET) {
            client = accept(listenSock, nullptr, nullptr);
            if (client == INVALID_SOCKET) Sleep(30);
        }
        if (client == INVALID_SOCKET) return;
        ioctlsocket(client, FIONBIO, &nonBlocking);
        {
            std::lock_guard<std::mutex> lock(net.mutex);
            net.socket = client;
            net.connected = true;
        }

        uint32_t lastSeq = 0;
        DWORD lastTick = GetTickCount();
        while (net.running) {
            InputFrame input{};
            int got = recv(client, reinterpret_cast<char*>(&input), sizeof(input), 0);
            if (got == sizeof(input) && input.magic == kMagic && input.version == kVersion) {
                std::lock_guard<std::mutex> lock(net.mutex);
                net.clientInput = input;
                net.clientInputReady = true;
                if (input.sequence != lastSeq) {
                    lastSeq = input.sequence;
                    net.latencyMs = static_cast<int>(GetTickCount() - lastTick);
                    lastTick = GetTickCount();
                }
            }
            WorldSnapshot snap{};
            {
                std::lock_guard<std::mutex> lock(net.mutex);
                snap = net.snapshot;
            }
            if (snap.magic == kMagic) {
                int sent = send(client, reinterpret_cast<const char*>(&snap), sizeof(snap), 0);
                if (sent == SOCKET_ERROR && WSAGetLastError() != WSAEWOULDBLOCK) break;
            }
            Sleep(12);
        }
        closesocket(client);
        {
            std::lock_guard<std::mutex> lock(net.mutex);
            net.socket = INVALID_SOCKET;
            net.connected = false;
        }
    }

    void clientThread() {
        WSADATA data{};
        if (WSAStartup(MAKEWORD(2, 2), &data) != 0) return;
        SOCKET sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (sock == INVALID_SOCKET) return;
        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_port = htons(kPort);
        addr.sin_addr.s_addr = inet_addr(joinIp);
        if (connect(sock, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == SOCKET_ERROR) {
            closesocket(sock);
            return;
        }
        u_long nonBlocking = 1;
        ioctlsocket(sock, FIONBIO, &nonBlocking);
        {
            std::lock_guard<std::mutex> lock(net.mutex);
            net.socket = sock;
            net.connected = true;
        }
        while (net.running) {
            WorldSnapshot snap{};
            int got = recv(sock, reinterpret_cast<char*>(&snap), sizeof(snap), 0);
            if (got == sizeof(snap) && snap.magic == kMagic && snap.version == kVersion) {
                std::lock_guard<std::mutex> lock(net.mutex);
                net.snapshot = snap;
                net.connected = true;
            } else if (got == 0) {
                break;
            }
            Sleep(8);
        }
        closesocket(sock);
        {
            std::lock_guard<std::mutex> lock(net.mutex);
            net.socket = INVALID_SOCKET;
            net.connected = false;
        }
    }

    void sendClientInput(const InputFrame& input) {
        SOCKET sock = INVALID_SOCKET;
        {
            std::lock_guard<std::mutex> lock(net.mutex);
            sock = net.socket;
        }
        if (sock != INVALID_SOCKET) {
            send(sock, reinterpret_cast<const char*>(&input), sizeof(input), 0);
        }
    }

    void drawText(HDC hdc, int x, int y, const char* text, COLORREF color, int size = 18) {
        SetBkMode(hdc, TRANSPARENT);
        SetTextColor(hdc, color);
        wchar_t wideText[512]{};
        MultiByteToWideChar(CP_UTF8, 0, text, -1, wideText, 512);
        HFONT font = CreateFontW(size, 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                                 OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, DEFAULT_QUALITY,
                                 DEFAULT_PITCH | FF_SWISS, L"Microsoft YaHei UI");
        HFONT old = static_cast<HFONT>(SelectObject(hdc, font));
        TextOutW(hdc, x, y, wideText, static_cast<int>(wcslen(wideText)));
        SelectObject(hdc, old);
        DeleteObject(font);
    }

    void fillRect(HDC hdc, int l, int t, int r, int b, COLORREF color) {
        HBRUSH brush = CreateSolidBrush(color);
        RECT rc{l, t, r, b};
        FillRect(hdc, &rc, brush);
        DeleteObject(brush);
    }

    void ellipse(HDC hdc, float x, float y, float radius, COLORREF fill, COLORREF stroke) {
        HBRUSH brush = CreateSolidBrush(fill);
        HPEN pen = CreatePen(PS_SOLID, 2, stroke);
        HGDIOBJ oldBrush = SelectObject(hdc, brush);
        HGDIOBJ oldPen = SelectObject(hdc, pen);
        Ellipse(hdc, static_cast<int>(x - radius), static_cast<int>(y - radius),
                static_cast<int>(x + radius), static_cast<int>(y + radius));
        SelectObject(hdc, oldBrush);
        SelectObject(hdc, oldPen);
        DeleteObject(brush);
        DeleteObject(pen);
    }

    void drawMenu(HDC hdc) {
        for (int i = 0; i < 18; ++i) {
            fillRect(hdc, i * 70, 80 + (i % 4) * 34, i * 70 + 42, 84 + (i % 4) * 34, RGB(37, 48, 73));
        }
        fillRect(hdc, 64, 190, 516, 642, RGB(18, 23, 36));
        fillRect(hdc, 594, 218, 1008, 552, RGB(18, 23, 36));
        drawText(hdc, 72, 86, u8"影域博弈", RGB(245, 226, 155), 54);
        drawText(hdc, 76, 154, u8"双人联机 2D 非对称对抗：光刃使 对战 影行者", RGB(184, 209, 228), 22);
        drawText(hdc, 96, 230, u8"[H] 建立房间：操控光刃使", RGB(254, 244, 188), 28);
        drawText(hdc, 96, 285, u8"[J] 加入房间：操控影行者", RGB(186, 210, 255), 28);
        drawText(hdc, 96, 340, u8"[O] 本机离线双人演示", RGB(208, 240, 202), 28);
        fillRect(hdc, 88, 386, 318, 430, RGB(65, 82, 120));
        drawText(hdc, 116, 397, u8"详细说明", RGB(255, 246, 196), 22);
        char ipLine[128];
        std::snprintf(ipLine, sizeof(ipLine), u8"加入 IP：%s   （输入数字和点修改，退格删除）", joinIp);
        drawText(hdc, 96, 464, ipLine, RGB(222, 222, 222), 18);
        drawText(hdc, 96, 520, u8"光刃使：W/A/S/D 移动，F 攻击，G 强光爆发，Q 放置光源，R 重开", RGB(218, 218, 218), 16);
        drawText(hdc, 96, 552, u8"离线影行者：方向键移动，Enter 攻击，K/Shift 冲刺，L/Ctrl 影域", RGB(218, 218, 218), 16);

        drawText(hdc, 626, 244, u8"点击“详细说明”查看：", RGB(245, 226, 155), 24);
        drawText(hdc, 642, 298, u8"联机步骤、角色技能、胜负规则", RGB(222, 226, 236), 18);
        drawText(hdc, 642, 336, u8"画面元素、常见问题、课堂展示建议", RGB(222, 226, 236), 18);
        drawText(hdc, 642, 398, u8"说明页可点击返回，也可按 Esc / B 返回。", RGB(208, 240, 202), 17);
    }

    void drawGuide(HDC hdc) {
        fillRect(hdc, 34, 36, 1086, 682, RGB(18, 23, 36));
        fillRect(hdc, 52, 622, 220, 664, RGB(65, 82, 120));
        drawText(hdc, 54, 52, u8"详细游玩声明", RGB(245, 226, 155), 34);
        drawText(hdc, 82, 632, u8"返回主菜单", RGB(255, 246, 196), 20);
        drawText(hdc, 248, 635, u8"也可以按 Esc / B / Backspace 返回", RGB(184, 209, 228), 16);

        const char* lines[] = {
            u8"一、项目定位：本游戏是 C++ 2D 双人联机 PVP，光刃使与影行者进行非对称对抗。",
            u8"二、进入方式：主机按 H 建房，客户端输入主机 IP 后按 J 加入；按 O 可本机离线双人演示。",
            u8"三、本机双开：第一个窗口按 H，第二个窗口保持 127.0.0.1 并按 J，即可测试联机同步。",
            u8"四、两台电脑：两台机器需在同一局域网；客户端输入主机局域网 IP；游戏使用 54000 端口。",
            u8"五、光刃使：W/A/S/D 移动，F 光弹，G 强光爆发，Q 放置光源，R 重开；亮处作战更强。",
            u8"六、影行者：联机时 W/A/S/D 移动，F 暗影弹，G 冲刺，Q 影域；暗处更灵活，适合突袭。",
            u8"七、离线影行者：方向键移动，Enter 攻击，K 或 Shift 冲刺，L 或 Ctrl 释放影域。",
            u8"八、胜负规则：任意一方先获得 3 分获胜；击败对手或控制中心据点都可以获得分数。",
            u8"九、占点规则：地图中央大圆是据点；单方站入会推动进度，双方同时进入则进入争夺状态。",
            u8"十、光影规则：黄色区域是光源；光刃使围绕光源压制，影行者用影域削弱光源再找机会切入。",
            u8"十一、画面元素：黄色角色是光刃使，蓝色角色是影行者，小圆点是攻击弹体，底部显示技能冷却。",
            u8"十二、联机同步：主机同步玩家位置、生命值、弹体、灯源、比分、占点进度和胜负结算。",
            u8"十三、常见问题：连不上时检查 IP、局域网、防火墙、端口 54000，以及主机是否已经按 H。",
            u8"十四、课堂展示：建议先按 O 离线展示技能，再双开演示联机，最后展示占点、击杀和胜负结算。",
            u8"十五、版本边界：当前是基础可玩版，可继续扩展贴图、音效、地图障碍、房间界面和断线提示。"
        };
        for (int i = 0; i < static_cast<int>(sizeof(lines) / sizeof(lines[0])); ++i) {
            drawText(hdc, 58, 112 + i * 34, lines[i], RGB(222, 226, 236), 17);
        }
    }

    void drawWorld(HDC hdc, const WorldSnapshot& w) {
        fillRect(hdc, static_cast<int>(kArenaLeft), static_cast<int>(kArenaTop),
                 static_cast<int>(kArenaRight), static_cast<int>(kArenaBottom), RGB(22, 26, 39));
        HPEN gridPen = CreatePen(PS_SOLID, 1, RGB(40, 48, 66));
        HGDIOBJ oldPen = SelectObject(hdc, gridPen);
        for (int x = 80; x < kArenaRight; x += 80) {
            MoveToEx(hdc, x, static_cast<int>(kArenaTop), nullptr);
            LineTo(hdc, x, static_cast<int>(kArenaBottom));
        }
        for (int y = 120; y < kArenaBottom; y += 80) {
            MoveToEx(hdc, static_cast<int>(kArenaLeft), y, nullptr);
            LineTo(hdc, static_cast<int>(kArenaRight), y);
        }
        SelectObject(hdc, oldPen);
        DeleteObject(gridPen);

        ellipse(hdc, 560.0f, 360.0f, 105.0f, RGB(35, 42, 58), RGB(112, 129, 165));
        float capWidth = w.capture >= 0 ? w.capture * 2.2f : -w.capture * 2.2f;
        if (w.capture >= 0) fillRect(hdc, 560, 56, 560 + static_cast<int>(capWidth), 66, RGB(246, 213, 96));
        else fillRect(hdc, 560 - static_cast<int>(capWidth), 56, 560, 66, RGB(108, 145, 255));

        for (int i = 0; i < 3; ++i) {
            if (!w.lights[i].active) continue;
            ellipse(hdc, w.lights[i].x, w.lights[i].y, w.lights[i].radius, RGB(68, 63, 36), RGB(216, 185, 70));
            ellipse(hdc, w.lights[i].x, w.lights[i].y, 8.0f, RGB(255, 235, 130), RGB(255, 255, 210));
        }
        for (int i = 0; i < 32; ++i) {
            const ProjectileState& b = w.projectiles[i];
            if (!b.active) continue;
            if (b.kind == 2) ellipse(hdc, b.x, b.y, 74.0f * clampValue(b.life, 0.0f, 0.65f), RGB(116, 99, 45), RGB(255, 228, 112));
            else if (b.kind == 3) ellipse(hdc, b.x, b.y, 66.0f * clampValue(b.life, 0.0f, 0.65f), RGB(28, 23, 52), RGB(116, 106, 210));
            else ellipse(hdc, b.x, b.y, 8.0f, b.kind == 0 ? RGB(252, 221, 99) : RGB(124, 146, 255), RGB(255, 255, 255));
        }
        drawPlayer(hdc, w.players[0], u8"光刃使", RGB(246, 213, 96), RGB(255, 246, 196));
        drawPlayer(hdc, w.players[1], u8"影行者", RGB(108, 145, 255), RGB(201, 214, 255));
        drawHud(hdc, w);
    }

    void drawPlayer(HDC hdc, const PlayerState& p, const char* name, COLORREF fill, COLORREF stroke) {
        ellipse(hdc, p.x, p.y, 22.0f, fill, stroke);
        fillRect(hdc, static_cast<int>(p.x - 32), static_cast<int>(p.y - 42),
                 static_cast<int>(p.x + 32), static_cast<int>(p.y - 36), RGB(54, 20, 25));
        fillRect(hdc, static_cast<int>(p.x - 32), static_cast<int>(p.y - 42),
                 static_cast<int>(p.x - 32 + 64.0f * clampValue(p.hp / 100.0f, 0.0f, 1.0f)), static_cast<int>(p.y - 36),
                 RGB(86, 214, 131));
        drawText(hdc, static_cast<int>(p.x - 26), static_cast<int>(p.y + 28), name, RGB(232, 232, 232), 14);
    }

    void drawHud(HDC hdc, const WorldSnapshot& w) {
        char line[256];
        std::snprintf(line, sizeof(line), u8"光刃使 %d  :  %d 影行者    占点 %.0f    时间 %.1f 秒",
                      w.players[0].score, w.players[1].score, w.capture, w.matchTime);
        drawText(hdc, 40, 18, line, RGB(236, 238, 242), 22);
        const char* state = u8"本机离线演示";
        if (mode == ModeHost) state = w.connected ? u8"主机：对手已连接" : u8"主机：等待对手连接 54000 端口";
        if (mode == ModeClient) state = u8"客户端：已连接主机";
        drawText(hdc, 760, 20, state, RGB(182, 205, 224), 18);
        std::snprintf(line, sizeof(line), u8"光刃冷却  攻击 %.1f  强光 %.1f  光源 %.1f",
                      w.players[0].attackCd, w.players[0].skillCd, w.players[0].utilityCd);
        drawText(hdc, 44, 684, line, RGB(245, 226, 155), 17);
        std::snprintf(line, sizeof(line), u8"影行冷却  攻击 %.1f  冲刺 %.1f  影域 %.1f",
                      w.players[1].attackCd, w.players[1].skillCd, w.players[1].utilityCd);
        drawText(hdc, 624, 684, line, RGB(190, 209, 255), 17);
        if (w.phase == PhaseWaiting) {
            drawText(hdc, 390, 320, u8"正在等待对手连接……", RGB(255, 255, 255), 34);
        }
        if (w.phase == PhaseGameOver) {
            drawText(hdc, 402, 306, w.winner == 0 ? u8"光刃使胜利" : u8"影行者胜利", RGB(255, 246, 190), 48);
            drawText(hdc, 432, 366, u8"按 R 重新开始对局", RGB(236, 238, 242), 22);
        }
    }
};

App* gApp = nullptr;

LRESULT CALLBACK windowProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
    case WM_KEYDOWN:
        if (gApp) gApp->onKeyDown(wParam);
        return 0;
    case WM_KEYUP:
        if (gApp) gApp->onKeyUp(wParam);
        return 0;
    case WM_LBUTTONDOWN:
        if (gApp) gApp->onMouseDown(LOWORD(lParam), HIWORD(lParam));
        InvalidateRect(hwnd, nullptr, FALSE);
        return 0;
    case WM_TIMER:
        if (gApp) gApp->tick();
        InvalidateRect(hwnd, nullptr, FALSE);
        return 0;
    case WM_PAINT: {
        PAINTSTRUCT ps{};
        HDC hdc = BeginPaint(hwnd, &ps);
        HDC mem = CreateCompatibleDC(hdc);
        HBITMAP bmp = CreateCompatibleBitmap(hdc, kWidth, kHeight);
        HGDIOBJ old = SelectObject(mem, bmp);
        if (gApp) gApp->draw(mem);
        BitBlt(hdc, 0, 0, kWidth, kHeight, mem, 0, 0, SRCCOPY);
        SelectObject(mem, old);
        DeleteObject(bmp);
        DeleteDC(mem);
        EndPaint(hwnd, &ps);
        return 0;
    }
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    default:
        return DefWindowProc(hwnd, msg, wParam, lParam);
    }
}

} // namespace

int WINAPI WinMain(HINSTANCE instance, HINSTANCE, LPSTR, int show) {
    App app;
    gApp = &app;

    WNDCLASSW wc{};
    wc.lpfnWndProc = windowProc;
    wc.hInstance = instance;
    wc.lpszClassName = L"YingYuBoYiWindow";
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    RegisterClassW(&wc);

    RECT rect{0, 0, kWidth, kHeight};
    AdjustWindowRect(&rect, WS_OVERLAPPEDWINDOW, FALSE);
    HWND hwnd = CreateWindowW(wc.lpszClassName, L"影域博弈 - 双人联机 2D PVP",
                              WS_OVERLAPPEDWINDOW & ~WS_MAXIMIZEBOX,
                              CW_USEDEFAULT, CW_USEDEFAULT,
                              rect.right - rect.left, rect.bottom - rect.top,
                              nullptr, nullptr, instance, nullptr);
    if (!hwnd) return 1;
    ShowWindow(hwnd, show);
    SetTimer(hwnd, 1, 16, nullptr);

    MSG msg{};
    while (GetMessage(&msg, nullptr, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }
    gApp = nullptr;
    return 0;
}
