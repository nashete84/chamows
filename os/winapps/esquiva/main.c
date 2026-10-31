/* Esquiva: a small Windows game (Win32 + GDI). Move with the arrows, dodge the meteors.
 * Build: x86_64-w64-mingw32-gcc -mwindows main.c -o esquiva.exe -lgdi32 */
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>

#define W 480
#define H 560
#define N 12

static struct { int x, y, r, speed; } rocks[N];
static int ship_x = W / 2, score, best, over;
static DWORD start;

static void new_rock(int i, int top) {
    rocks[i].r = 10 + rand() % 18;
    rocks[i].x = rocks[i].r + rand() % (W - 2 * rocks[i].r);
    rocks[i].y = top ? -rocks[i].r - rand() % H : -rocks[i].r;
    rocks[i].speed = 3 + rand() % 4 + score / 400;
}

static void reset(void) {
    for (int i = 0; i < N; i++) new_rock(i, 1);
    ship_x = W / 2;
    score = 0;
    over = 0;
    start = GetTickCount();
}

static void step(void) {
    if (over) return;
    if (GetAsyncKeyState(VK_LEFT) & 0x8000) ship_x -= 9;
    if (GetAsyncKeyState(VK_RIGHT) & 0x8000) ship_x += 9;
    if (ship_x < 20) ship_x = 20;
    if (ship_x > W - 20) ship_x = W - 20;
    score++;
    for (int i = 0; i < N; i++) {
        rocks[i].y += rocks[i].speed;
        if (rocks[i].y - rocks[i].r > H) new_rock(i, 0);
        int dx = rocks[i].x - ship_x, dy = rocks[i].y - (H - 50);
        int rr = rocks[i].r + 14;
        if (dx * dx + dy * dy < rr * rr) {
            over = 1;
            if (score > best) best = score;
        }
    }
}

static void paint(HDC dc) {
    HDC mem = CreateCompatibleDC(dc);
    HBITMAP bmp = CreateCompatibleBitmap(dc, W, H);
    HGDIOBJ old = SelectObject(mem, bmp);
    RECT all = {0, 0, W, H};
    HBRUSH sky = CreateSolidBrush(RGB(18, 15, 46));
    FillRect(mem, &all, sky);
    DeleteObject(sky);
    /* stars */
    for (int i = 0; i < 40; i++) SetPixel(mem, (i * 97) % W, (i * 53 + score / 2) % H, RGB(200, 200, 255));
    /* meteors */
    HBRUSH rock = CreateSolidBrush(RGB(255, 150, 80));
    HPEN edge = CreatePen(PS_SOLID, 2, RGB(255, 210, 120));
    SelectObject(mem, rock);
    SelectObject(mem, edge);
    for (int i = 0; i < N; i++)
        Ellipse(mem, rocks[i].x - rocks[i].r, rocks[i].y - rocks[i].r, rocks[i].x + rocks[i].r, rocks[i].y + rocks[i].r);
    /* ship */
    HBRUSH ship = CreateSolidBrush(RGB(120, 200, 255));
    SelectObject(mem, ship);
    SelectObject(mem, GetStockObject(NULL_PEN));
    RoundRect(mem, ship_x - 16, H - 62, ship_x + 16, H - 34, 12, 12);
    Rectangle(mem, ship_x - 4, H - 72, ship_x + 4, H - 60);
    /* text */
    char line[64];
    SetBkMode(mem, TRANSPARENT);
    SetTextColor(mem, RGB(255, 255, 255));
    sprintf(line, "Puntos: %d   Récord: %d", score / 10, best / 10);
    TextOutA(mem, 12, 10, line, lstrlenA(line));
    if (over) {
        RECT box = {60, 220, W - 60, 320};
        HBRUSH panel = CreateSolidBrush(RGB(40, 44, 80));
        FillRect(mem, &box, panel);
        DeleteObject(panel);
        RECT t1 = {60, 236, W - 60, 266}, t2 = {60, 276, W - 60, 306};
        DrawTextA(mem, "¡Te ha dado un meteorito!", -1, &t1, DT_CENTER | DT_SINGLELINE);
        DrawTextA(mem, "Pulsa Espacio para jugar otra vez", -1, &t2, DT_CENTER | DT_SINGLELINE);
    }
    BitBlt(dc, 0, 0, W, H, mem, 0, 0, SRCCOPY);
    SelectObject(mem, old);
    DeleteObject(bmp);
    DeleteObject(rock);
    DeleteObject(edge);
    DeleteObject(ship);
    DeleteDC(mem);
}

static LRESULT CALLBACK proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_CREATE:
        SetTimer(hwnd, 1, 30, NULL);
        return 0;
    case WM_TIMER:
        step();
        InvalidateRect(hwnd, NULL, FALSE);
        return 0;
    case WM_KEYDOWN:
        if (wp == VK_LEFT) ship_x -= 12;
        if (wp == VK_RIGHT) ship_x += 12;
        if (wp == VK_SPACE && over) reset();
        if (wp == VK_ESCAPE) DestroyWindow(hwnd);
        return 0;
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(hwnd, &ps);
        paint(dc);
        EndPaint(hwnd, &ps);
        return 0;
    }
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcA(hwnd, msg, wp, lp);
}

int WINAPI WinMain(HINSTANCE inst, HINSTANCE prev, LPSTR cmd, int show) {
    srand(GetTickCount());
    reset();
    WNDCLASSA wc = {0};
    wc.lpfnWndProc = proc;
    wc.hInstance = inst;
    wc.hCursor = LoadCursorA(NULL, IDC_ARROW);
    wc.lpszClassName = "Esquiva";
    RegisterClassA(&wc);
    RECT r = {0, 0, W, H};
    AdjustWindowRect(&r, WS_OVERLAPPEDWINDOW, FALSE);
    HWND hwnd = CreateWindowExA(0, "Esquiva", "Esquiva", WS_OVERLAPPEDWINDOW | WS_VISIBLE, CW_USEDEFAULT, CW_USEDEFAULT,
                                r.right - r.left, r.bottom - r.top, NULL, NULL, inst, NULL);
    ShowWindow(hwnd, show);
    UpdateWindow(hwnd);
    MSG msg;
    while (GetMessageA(&msg, NULL, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageA(&msg);
    }
    return (int)msg.wParam;
}
