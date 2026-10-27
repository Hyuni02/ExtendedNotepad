// 더 나은 메모장 (Extended Notepad)
// Phase 1: 분할 레이아웃 + 편집 기본
// Phase 2: .etxt/.txt 파일 I/O + 전역 undo 스택
// Phase 3: 찾기/바꾸기/이동, 확대·축소, 상태 표시줄, 인쇄, 설정
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <windowsx.h>
#include <commctrl.h>
#include <commdlg.h>
#include <shellapi.h>
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <string>
#include <vector>
#include "resource.h"

#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "comdlg32.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "user32.lib")
#pragma comment(lib, "gdi32.lib")

// ---- 상수 ------------------------------------------------------------------
enum { DIR_V = 0, DIR_H = 1 };   // V: 수직 분할선(좌/우), H: 수평 분할선(상/하)
static const int GAP    = 2;     // SPEC §3: Pane 사이 간격 2px
static const int HITPAD = 3;     // 분할선 드래그 히트 영역 확장 (2px는 집기 어려움)

enum { CMD_TEXT = 0, CMD_SPLIT = 1, CMD_CLOSE = 2 };
enum { ENC_UTF8 = 0, ENC_UTF8BOM, ENC_UTF16LE, ENC_ANSI };

enum {
    IDM_NEW = 100, IDM_NEWWIN, IDM_OPEN, IDM_SAVE, IDM_SAVEAS, IDM_EXIT,
    IDM_PAGESETUP, IDM_PRINT,
    IDM_SPLIT_V, IDM_SPLIT_H, IDM_CLOSE_PANE, IDM_SAVE_PANE,
    IDM_NEXT_PANE, IDM_PREV_PANE,
    IDM_UNDO, IDM_REDO,
    IDM_CUT, IDM_COPY, IDM_PASTE, IDM_DELETE, IDM_SELECT_ALL, IDM_TIMEDATE,
    IDM_FIND, IDM_FINDNEXT, IDM_FINDPREV, IDM_REPLACE, IDM_GOTO, IDM_BINGSEARCH,
    IDM_WORDWRAP, IDM_FONT, IDM_SETTINGS,
    IDM_ZOOMIN, IDM_ZOOMOUT, IDM_ZOOMRESET, IDM_STATUSBAR, IDM_ABOUT,
    IDM_ENC_FIRST, IDM_ENC_UTF8 = IDM_ENC_FIRST, IDM_ENC_UTF8BOM,
    IDM_ENC_UTF16LE, IDM_ENC_ANSI, IDM_ENC_LAST = IDM_ENC_ANSI,
    IDC_EDIT_FIRST = 2000
};

static const wchar_t* const kEncNames[] = { L"UTF-8", L"UTF-8 (BOM)", L"UTF-16 LE", L"ANSI" };

// ---- 트리 ------------------------------------------------------------------
struct Node {
    bool  isPane;
    int   id;        // Pane 식별자 (undo 명령이 참조)
    Node* parent;
    RECT  rc;        // 배치된 영역
    HWND  edit;      // Pane
    bool  vsb, hsb;  // 스크롤바를 지금 보여주고 있는가
    int   dir;       // Split
    float ratio;
    Node* a;
    Node* b;
    RECT  sp;        // 분할선 영역 (Split일 때만 유효)
};

typedef std::vector<unsigned char> Path;   // 루트에서 0=a, 1=b

// ---- 명령 (전역 undo 스택) -------------------------------------------------
struct Cmd {
    int kind;
    // CMD_TEXT
    int pane, pos, selA, selB;
    std::wstring del, ins;
    // CMD_SPLIT / CMD_CLOSE
    Path  path;          // SPLIT: 생성된 Split 노드, CLOSE: 사라진 Split 노드 자리
    int   dir;
    float ratio;
    bool  wasA;          // CLOSE: 제거된 Pane이 a쪽이었는가
    std::wstring text;   // CLOSE: 제거된 Pane의 내용
    Cmd() : kind(0), pane(0), pos(0), selA(0), selB(0), dir(0), ratio(0.5f), wasA(false) {}
};

static Node* g_root  = NULL;
static Node* g_focus = NULL;     // 포커스 Pane (항상 leaf)
static HWND  g_main  = NULL;
static HFONT g_font  = NULL;
static bool  g_wrap  = true;     // SPEC: 창 전체 적용
static bool  g_dirty = false;
static bool  g_quiet = false;    // 컨트롤 재생성 중 알림 무시
static bool  g_applying = false; // undo/redo 적용 중에는 기록하지 않음
static int   g_nextEditId = IDC_EDIT_FIRST;
static int   g_nextPaneId = 1;
static Node* g_drag  = NULL;     // 드래그 중인 Split 노드
static int   g_enc   = ENC_UTF8;
static std::wstring g_path;      // 현재 파일 (빈 문자열 = 제목 없음)

static std::vector<Cmd> g_cmds;
static size_t    g_pos = 0;        // 적용된 명령 수
static long long g_savePoint = 0;  // 저장 시점 (-1 = 도달 불가)
static bool      g_runOpen = false;// 타이핑 묶음 진행 중

// Phase 3
static HWND    g_status = NULL;
static bool    g_statusOn = true;
static LOGFONTW g_baseFont;        // 글꼴 모양 (크기는 g_basePt)
static int     g_basePt = 11;
static int     g_zoom = 100;       // 퍼센트, 창 전체 적용
static int     g_lineH = 16;       // 현재 글꼴의 한 줄 높이 (스크롤바 판정용)
static HWND    g_findDlg = NULL;   // 찾기 또는 바꾸기 (메모장처럼 한 번에 하나)
static bool    g_findIsReplace = false;
static PAGESETUPDLGW g_psd;
static bool    g_psdInit = false;
static RECT    g_place = { 0, 0, 0, 0 };   // 마지막으로 쓴 창 위치/크기
static bool    g_placeMax = false;         // 최대화 상태였는가
static bool    g_hasPlace = false;

struct FindOpts {
    std::wstring what, with;
    bool matchCase, wrapAround, allPanes, up;
    FindOpts() : matchCase(false), wrapAround(true), allPanes(false), up(false) {}
};
static FindOpts g_find;

static LRESULT CALLBACK EditProc(HWND, UINT, WPARAM, LPARAM, UINT_PTR, DWORD_PTR);
static void UpdateStatus();
static std::wstring PaneText(Node* n);

// ---- 트리 기본 -------------------------------------------------------------
static Node* NewSplit(int dir) {
    Node* n = new Node();
    n->isPane = false; n->dir = dir; n->ratio = 0.5f;
    return n;
}

static HWND CreateEditCtrl() {
    if (!g_main) return NULL;    // selftest: HWND 없이 트리만 검증
    DWORD st = WS_CHILD | WS_VISIBLE | WS_VSCROLL | ES_MULTILINE |
               ES_NOHIDESEL | ES_WANTRETURN | ES_AUTOVSCROLL;
    if (!g_wrap) st |= WS_HSCROLL | ES_AUTOHSCROLL;
    HWND h = CreateWindowExW(0, L"EDIT", L"", st, 0, 0, 0, 0,
                             g_main, (HMENU)(INT_PTR)g_nextEditId++,
                             GetModuleHandleW(NULL), NULL);
    SendMessageW(h, EM_SETLIMITTEXT, 0, 0);   // 기본 길이 제한 해제
    if (g_font) SendMessageW(h, WM_SETFONT, (WPARAM)g_font, TRUE);
    SetWindowSubclass(h, EditProc, 1, 0);
    return h;
}

static Node* NewPane() {
    Node* n = new Node();
    n->isPane = true;
    n->id = g_nextPaneId++;
    n->edit = CreateEditCtrl();
    n->vsb = n->hsb = true;     // 스크롤 스타일로 만들어져 처음엔 보인다
    return n;
}

// 가로는 "가장 긴 줄의 픽셀 폭"을 알아야 하고, 세로처럼 공짜로 얻을 방법이 없다.
// 줄마다 재므로 문서 크기에 비례한다 → 일정 크기를 넘으면 재지 않고 그냥 보여준다.
// ponytail: 한도를 넘는 문서에서는 예전처럼 항상 보인다. 더 필요해지면
// 바뀐 줄만 재고 최대 폭을 캐시하는 쪽으로 올린다.
static const int kHScanLimit = 256 * 1024;

static bool NeedHScroll(Node* n) {
    if (GetWindowTextLengthW(n->edit) > kHScanLimit) return true;
    RECT rc;
    GetClientRect(n->edit, &rc);
    std::wstring t = PaneText(n);
    HDC dc = GetDC(n->edit);
    HFONT prev = (HFONT)SelectObject(dc, g_font);
    int widest = 0;
    size_t i = 0;
    for (;;) {
        size_t e = t.find(L"\r\n", i);
        size_t end = (e == std::wstring::npos) ? t.size() : e;
        if (end > i) {
            // EDIT은 탭을 펼치므로 탭을 아는 쪽으로 잰다
            DWORD w = (DWORD)GetTabbedTextExtentW(dc, t.c_str() + i, (int)(end - i), 0, NULL);
            if ((int)LOWORD(w) > widest) widest = (int)LOWORD(w);
        }
        if (e == std::wstring::npos) break;
        i = e + 2;
    }
    SelectObject(dc, prev);
    ReleaseDC(n->edit, dc);
    return widest > (rc.right - rc.left) - 4;   // EDIT 내부 여백만큼 여유
}

// EDIT은 WS_VSCROLL을 주면 스크롤이 필요 없을 때도 스크롤바를 계속 보여준다.
// 직접 숨기되, 판정은 스크롤바 자체에 묻지 않는다 — 한 번 숨기면 GetScrollInfo가
// 실패해서 다시 보여줄 판단을 못 하게 된다. 줄 수와 보이는 줄 수로만 따진다.
// 세로 스크롤바는 폭에만 영향을 주므로 숨기면 (줄바꿈 시) 줄 수가 줄어들 뿐이어서
// 보임/숨김이 진동하지 않는다.
static void UpdateScrollbars(Node* n) {
    if (!n->isPane || !n->edit) return;
    RECT rc;
    GetClientRect(n->edit, &rc);
    int lh = g_lineH > 0 ? g_lineH : 1;
    int visible = (rc.bottom - rc.top) / lh;
    if (visible < 1) visible = 1;
    int lines = (int)SendMessageW(n->edit, EM_GETLINECOUNT, 0, 0);
    bool need = lines > visible;
    if (need != n->vsb) {                            // 바뀔 때만 건드린다
        n->vsb = need;
        ShowScrollBar(n->edit, SB_VERT, need);
    }
    // 자동 줄 바꿈이 켜져 있으면 가로 스크롤바 자체가 없다
    if (!g_wrap) {
        bool needH = NeedHScroll(n);
        if (needH != n->hsb) {
            n->hsb = needH;
            ShowScrollBar(n->edit, SB_HORZ, needH);
        }
    }
}

static void UpdateScrollbarsAll(Node* n) {
    if (n->isPane) { UpdateScrollbars(n); return; }
    UpdateScrollbarsAll(n->a);
    UpdateScrollbarsAll(n->b);
}

static Node* FirstLeaf(Node* n) { while (!n->isPane) n = n->a; return n; }

// SPEC §3: 트리 선순위 순회 = 좌→우, 위→아래
static void CollectLeaves(Node* n, std::vector<Node*>& out) {
    if (n->isPane) { out.push_back(n); return; }
    CollectLeaves(n->a, out);
    CollectLeaves(n->b, out);
}

static Node* FindByHwnd(Node* n, HWND h) {
    if (n->isPane) return n->edit == h ? n : NULL;
    Node* r = FindByHwnd(n->a, h);
    return r ? r : FindByHwnd(n->b, h);
}

static Node* FindById(Node* n, int id) {
    if (n->isPane) return n->id == id ? n : NULL;
    Node* r = FindById(n->a, id);
    return r ? r : FindById(n->b, id);
}

static void FreeSubtree(Node* n) {
    if (!n) return;
    if (n->isPane) { if (n->edit) DestroyWindow(n->edit); }
    else { FreeSubtree(n->a); FreeSubtree(n->b); }
    delete n;
}

static Path PathOf(Node* n) {
    Path p;
    for (Node* c = n; c->parent; c = c->parent)
        p.push_back(c->parent->a == c ? 0 : 1);
    // 위에서 아래로 뒤집는다
    for (size_t i = 0, j = p.size(); i + 1 < j; ++i, --j) {
        unsigned char t = p[i]; p[i] = p[j - 1]; p[j - 1] = t;
    }
    return p;
}

static Node* AtPath(const Path& p) {
    Node* n = g_root;
    for (size_t i = 0; i < p.size(); ++i) {
        if (!n || n->isPane) return NULL;
        n = p[i] ? n->b : n->a;
    }
    return n;
}

// path 위치의 노드를 repl로 치환하고 부모를 연결한다.
static void ReplaceAtPath(const Path& p, Node* repl) {
    if (p.empty()) { g_root = repl; repl->parent = NULL; return; }
    Path pp(p.begin(), p.end() - 1);
    Node* par = AtPath(pp);
    if (!par || par->isPane) return;
    if (p.back()) par->b = repl; else par->a = repl;
    repl->parent = par;
}

// ---- 배치 ------------------------------------------------------------------
static int LeafCount(Node* n) { return n->isPane ? 1 : LeafCount(n->a) + LeafCount(n->b); }

static HDWP g_hdwp = NULL;   // 배치를 묶는 중이면 여기에 쌓는다

static void Layout(Node* n, RECT r) {
    n->rc = r;
    if (n->isPane) {
        if (n->edit) {
            int w = r.right - r.left, h = r.bottom - r.top;
            if (g_hdwp) {
                HDWP d = DeferWindowPos(g_hdwp, n->edit, NULL, r.left, r.top, w, h,
                                        SWP_NOZORDER);
                if (d) g_hdwp = d;
            } else {
                SetWindowPos(n->edit, NULL, r.left, r.top, w, h, SWP_NOZORDER);
            }
        }
        return;
    }
    if (n->dir == DIR_V) {
        int avail = (r.right - r.left) - GAP;
        if (avail < 0) avail = 0;
        int x = r.left + (int)(avail * n->ratio + 0.5f);
        SetRect(&n->sp, x, r.top, x + GAP, r.bottom);
        RECT ra = { r.left, r.top, x, r.bottom };
        RECT rb = { x + GAP, r.top, r.right, r.bottom };
        if (rb.left > rb.right) rb.left = rb.right;
        Layout(n->a, ra); Layout(n->b, rb);
    } else {
        int avail = (r.bottom - r.top) - GAP;
        if (avail < 0) avail = 0;
        int y = r.top + (int)(avail * n->ratio + 0.5f);
        SetRect(&n->sp, r.left, y, r.right, y + GAP);
        RECT ra = { r.left, r.top, r.right, y };
        RECT rb = { r.left, y + GAP, r.right, r.bottom };
        if (rb.top > rb.bottom) rb.top = rb.bottom;
        Layout(n->a, ra); Layout(n->b, rb);
    }
}

// Pane을 하나씩 옮기면 중간 상태가 보인다. 한 번에 옮긴다.
static void LayoutBatch(Node* n, RECT r) {
    g_hdwp = BeginDeferWindowPos(LeafCount(n));
    Layout(n, r);                      // g_hdwp가 NULL이면 SetWindowPos로 떨어진다
    if (g_hdwp) { EndDeferWindowPos(g_hdwp); g_hdwp = NULL; }
    if (g_main) UpdateScrollbarsAll(n);   // 크기가 확정된 뒤에 판정한다
}

static void Relayout() {
    if (!g_root || !g_main) return;
    RECT rc; GetClientRect(g_main, &rc);
    if (g_status && g_statusOn) {
        RECT sb; GetWindowRect(g_status, &sb);
        rc.bottom -= (sb.bottom - sb.top);
        if (rc.bottom < rc.top) rc.bottom = rc.top;
    }
    LayoutBatch(g_root, rc);
    InvalidateRect(g_main, NULL, TRUE);
}

static Node* HitSplitter(Node* n, POINT pt) {
    if (n->isPane) return NULL;
    if (!PtInRect(&n->rc, pt)) return NULL;
    RECT h = n->sp;
    InflateRect(&h, n->dir == DIR_V ? HITPAD : 0, n->dir == DIR_H ? HITPAD : 0);
    if (PtInRect(&h, pt)) return n;
    Node* r = HitSplitter(n->a, pt);
    return r ? r : HitSplitter(n->b, pt);
}

// ---- Pane 텍스트 -----------------------------------------------------------
static std::wstring PaneText(Node* n) {
    if (!n->edit) return L"";
    int len = GetWindowTextLengthW(n->edit);
    std::wstring s((size_t)len + 1, L'\0');
    GetWindowTextW(n->edit, &s[0], len + 1);
    s.resize(len);
    return s;
}

static void GetSel(HWND h, int& a, int& b) {
    DWORD s = 0, e = 0;
    SendMessageW(h, EM_GETSEL, (WPARAM)&s, (LPARAM)&e);   // 포인터 형태만 32비트
    a = (int)s; b = (int)e;
}

// EDIT에는 EM_GETTEXTRANGE가 없다. 줄 단위로 읽어 범위를 모은다.
// ponytail: 선택 영역이 크면 그만큼 비용이 든다. undo에 그 텍스트가 필요하므로 불가피.
static std::wstring EditRange(HWND h, int from, int to) {
    int total = GetWindowTextLengthW(h);
    if (from < 0) from = 0;
    if (to > total) to = total;
    if (to <= from) return L"";
    int line  = (int)SendMessageW(h, EM_LINEFROMCHAR, from, 0);
    int start = (int)SendMessageW(h, EM_LINEINDEX, line, 0);
    if (start < 0) return L"";
    int idx = start;
    std::wstring s;
    std::vector<wchar_t> buf;
    for (;;) {
        int len = (int)SendMessageW(h, EM_LINELENGTH, idx, 0);
        if (len > 0) {
            buf.assign((size_t)len + 2, L'\0');
            *(WORD*)&buf[0] = (WORD)(len + 1);
            int got = (int)SendMessageW(h, EM_GETLINE, line, (LPARAM)&buf[0]);
            if (got > len) got = len;
            s.append(&buf[0], (size_t)got);
        }
        int next = (int)SendMessageW(h, EM_LINEINDEX, line + 1, 0);
        if (next < 0) break;          // 마지막 줄
        s.append(L"\r\n");            // Win10 EDIT의 줄 종결자는 항상 CRLF
        if (next >= to) break;
        ++line; idx = next;
    }
    size_t off = (size_t)(from - start);
    if (off >= s.size()) return L"";
    return s.substr(off, (size_t)(to - from));
}

// ---- 문서 상태 -------------------------------------------------------------
static const wchar_t* BaseName(const std::wstring& p) {
    size_t i = p.find_last_of(L"\\/");
    return i == std::wstring::npos ? p.c_str() : p.c_str() + i + 1;
}

static void UpdateTitle() {
    if (!g_main) return;
    std::wstring name = g_path.empty() ? L"제목 없음" : BaseName(g_path);
    std::wstring t = (g_dirty ? L"*" : L"") + name + L" - 더 나은 메모장";
    SetWindowTextW(g_main, t.c_str());
}

static void UpdateDirty() {
    bool d = ((long long)g_pos != g_savePoint);
    if (d == g_dirty) return;
    g_dirty = d;
    UpdateTitle();
}

static void BreakRun() { g_runOpen = false; }

static void ResetUndo() {
    g_cmds.clear();
    g_pos = 0;
    g_savePoint = 0;
    g_runOpen = false;
    g_dirty = false;
    UpdateTitle();
}

static void PushCmd(const Cmd& c) {
    if (g_pos < g_cmds.size()) {
        if (g_savePoint > (long long)g_pos) g_savePoint = -1;  // 저장 시점이 사라졌다
        g_cmds.resize(g_pos);
    }
    g_cmds.push_back(c);
    g_pos = g_cmds.size();
    UpdateDirty();
}

// ---- 분할 / 닫기 (원시 조작: 스택에 기록하지 않음) --------------------------
static Node* DoSplit(Node* p, int dir) {
    Node* s  = NewSplit(dir);
    Node* np = NewPane();
    s->parent = p->parent;
    s->a = p; s->b = np;
    if (!p->parent) g_root = s;
    else if (p->parent->a == p) p->parent->a = s;
    else p->parent->b = s;
    p->parent = s; np->parent = s;
    g_focus = np;
    Relayout();
    if (np->edit) SetFocus(np->edit);
    return s;
}

static void DoClose(Node* p) {
    Node* par = p->parent;
    if (!par) return;
    Node* sib = (par->a == p) ? par->b : par->a;
    Node* gp  = par->parent;
    if (!gp) { g_root = sib; sib->parent = NULL; }
    else {
        if (gp->a == par) gp->a = sib; else gp->b = sib;
        sib->parent = gp;
    }
    par->a = par->b = NULL;
    FreeSubtree(p);
    delete par;
    g_focus = FirstLeaf(sib);
    Relayout();
    if (g_focus->edit) SetFocus(g_focus->edit);
}

// ---- 분할 / 닫기 (명령) ----------------------------------------------------
static void SplitFocus(int dir) {
    if (!g_focus) return;
    BreakRun();
    Node* s = DoSplit(g_focus, dir);
    Cmd c; c.kind = CMD_SPLIT; c.path = PathOf(s); c.dir = dir;
    PushCmd(c);
}

static bool ConfirmSave();   // 전방 선언

static void CloseFocusPane() {
    Node* p = g_focus;
    if (!p) return;
    if (!p->parent) {           // SPEC §4: 마지막 Pane이면 창을 닫는다
        PostMessageW(g_main, WM_CLOSE, 0, 0);
        return;
    }
    BreakRun();
    Cmd c;
    c.kind  = CMD_CLOSE;
    c.path  = PathOf(p->parent);
    c.dir   = p->parent->dir;
    c.ratio = p->parent->ratio;
    c.wasA  = (p->parent->a == p);
    c.text  = PaneText(p);
    DoClose(p);
    PushCmd(c);
}

static void CyclePane(int delta) {
    std::vector<Node*> v;
    CollectLeaves(g_root, v);
    if (v.size() < 2) return;
    int i = 0;
    for (size_t k = 0; k < v.size(); ++k) if (v[k] == g_focus) i = (int)k;
    int n = (int)v.size();
    Node* t = v[(i + delta + n) % n];
    BreakRun();
    if (t->edit) SetFocus(t->edit);
}

// ---- 실행 취소 / 다시 실행 -------------------------------------------------
static void ApplyText(Node* n, int pos, const std::wstring& remove,
                      const std::wstring& insert, int selA, int selB) {
    if (!n->edit) return;
    g_focus = n;
    SetFocus(n->edit);
    SendMessageW(n->edit, EM_SETSEL, pos, pos + (int)remove.size());
    SendMessageW(n->edit, EM_REPLACESEL, FALSE, (LPARAM)insert.c_str());
    SendMessageW(n->edit, EM_SETSEL, selA, selB);
    SendMessageW(n->edit, EM_SCROLLCARET, 0, 0);
    UpdateScrollbars(n);
}

static void Undo() {
    if (g_pos == 0) return;
    BreakRun();
    const Cmd c = g_cmds[g_pos - 1];
    g_applying = true;
    if (c.kind == CMD_TEXT) {
        Node* n = FindById(g_root, c.pane);
        if (n) ApplyText(n, c.pos, c.ins, c.del, c.selA, c.selB);
    } else if (c.kind == CMD_SPLIT) {
        Node* s = AtPath(c.path);
        if (s && !s->isPane) {
            Node* keep = s->a;
            FreeSubtree(s->b);
            s->a = s->b = NULL;
            ReplaceAtPath(c.path, keep);
            delete s;
            g_focus = FirstLeaf(keep);
            Relayout();
            if (g_focus->edit) SetFocus(g_focus->edit);
        }
    } else {                      // CMD_CLOSE 취소: Split과 Pane을 되살린다
        Node* x = AtPath(c.path);
        if (x) {
            Node* s = NewSplit(c.dir);
            s->ratio = c.ratio;
            Node* r = NewPane();
            if (r->edit) {
                g_quiet = true;
                SetWindowTextW(r->edit, c.text.c_str());
                g_quiet = false;
            }
            if (c.wasA) { s->a = r; s->b = x; } else { s->a = x; s->b = r; }
            r->parent = s;
            ReplaceAtPath(c.path, s);
            x->parent = s;
            g_focus = r;
            Relayout();
            if (r->edit) SetFocus(r->edit);
        }
    }
    g_applying = false;
    g_pos--;
    UpdateDirty();
}

static void Redo() {
    if (g_pos >= g_cmds.size()) return;
    BreakRun();
    const Cmd c = g_cmds[g_pos];
    g_applying = true;
    if (c.kind == CMD_TEXT) {
        Node* n = FindById(g_root, c.pane);
        if (n) ApplyText(n, c.pos, c.del, c.ins,
                         c.pos + (int)c.ins.size(), c.pos + (int)c.ins.size());
    } else if (c.kind == CMD_SPLIT) {
        Node* p = AtPath(c.path);
        if (p && p->isPane) DoSplit(p, c.dir);
    } else {
        Node* s = AtPath(c.path);
        if (s && !s->isPane) DoClose(c.wasA ? s->a : s->b);
    }
    g_applying = false;
    g_pos++;
    UpdateDirty();
}

// 타이핑 묶음 병합 (SPEC §5)
static bool TryMerge(const Cmd& c) {
    if (!g_runOpen || g_pos == 0 || g_pos != g_cmds.size()) return false;
    Cmd& p = g_cmds[g_pos - 1];
    if (p.kind != CMD_TEXT || p.pane != c.pane) return false;
    // 1) 연속 입력. 묶음은 줄바꿈에서 끝난다 — 넣을 때도, 이미 넣은 뒤에도.
    if (c.del.empty() && p.del.empty() &&
        c.pos == p.pos + (int)p.ins.size() &&
        c.ins.find(L'\r') == std::wstring::npos &&
        (p.ins.empty() || p.ins[p.ins.size() - 1] != L'\n')) {
        p.ins += c.ins; return true;
    }
    // 2) 제자리 치환 — 한글 IME 조합이 글자를 계속 고쳐 쓴다
    if (c.pos == p.pos && c.del == p.ins) { p.ins = c.ins; return true; }
    // 3) 연속 백스페이스
    if (c.ins.empty() && p.ins.empty() && c.pos + (int)c.del.size() == p.pos) {
        p.del = c.del + p.del; p.pos = c.pos; return true;
    }
    return false;
}

// ---- 텍스트 편집 가로채기 --------------------------------------------------
// 텍스트를 바꿀 수 있는 메시지만 가로챈다. EDIT은 OLE 드래그 앤 드롭이 없어
// 편집은 항상 캐럿/선택 주변 한 묶음이다.
static bool GateMsg(UINT msg, WPARAM wp) {
    switch (msg) {
    case WM_CHAR: case WM_IME_CHAR: case WM_IME_COMPOSITION:
    case WM_PASTE: case WM_CUT: case WM_CLEAR: case EM_REPLACESEL:
        return true;
    case WM_KEYDOWN:
        if (wp == VK_BACK || wp == VK_DELETE || wp == VK_RETURN || wp == VK_TAB)
            return true;
        if ((GetKeyState(VK_CONTROL) & 0x8000) && (wp == 'V' || wp == 'X'))
            return true;
        return false;
    }
    return false;
}

static bool IsNavKey(WPARAM wp) {
    return wp == VK_LEFT || wp == VK_RIGHT || wp == VK_UP || wp == VK_DOWN ||
           wp == VK_HOME || wp == VK_END || wp == VK_PRIOR || wp == VK_NEXT;
}

struct Pre { int s0, e0, len0, a0, b0; std::wstring old; };

// 편집이 닿을 수 있는 범위를 줄 단위로 넉넉히 잡는다.
// 한 줄 앞뒤까지 보면 Ctrl+Backspace(단어 삭제)와 줄 경계 삭제를 모두 덮는다.
static Pre PreCapture(HWND h) {
    Pre p;
    GetSel(h, p.s0, p.e0);
    p.len0 = GetWindowTextLengthW(h);
    int l0 = (int)SendMessageW(h, EM_LINEFROMCHAR, p.s0, 0);
    int l1 = (int)SendMessageW(h, EM_LINEFROMCHAR, p.e0, 0);
    int a0 = (int)SendMessageW(h, EM_LINEINDEX, l0 > 0 ? l0 - 1 : 0, 0);
    int nb = (int)SendMessageW(h, EM_LINEINDEX, l1 + 2, 0);
    p.a0 = a0 < 0 ? 0 : a0;
    p.b0 = nb < 0 ? p.len0 : nb;
    p.old = EditRange(h, p.a0, p.b0);
    return p;
}

static void PostCapture(HWND h, Node* n, const Pre& p) {
    int len1 = GetWindowTextLengthW(h);
    std::wstring nw = EditRange(h, p.a0, p.b0 + (len1 - p.len0));
    if (nw == p.old) return;

    size_t m = p.old.size() < nw.size() ? p.old.size() : nw.size();
    size_t i = 0;
    while (i < m && p.old[i] == nw[i]) ++i;
    size_t j = 0;
    while (j < m - i && p.old[p.old.size() - 1 - j] == nw[nw.size() - 1 - j]) ++j;

    Cmd c;
    c.kind = CMD_TEXT;
    c.pane = n->id;
    c.pos  = (int)(p.a0 + i);
    c.del  = p.old.substr(i, p.old.size() - i - j);
    c.ins  = nw.substr(i, nw.size() - i - j);
    c.selA = p.s0; c.selB = p.e0;
    if (TryMerge(c)) UpdateDirty();
    else { PushCmd(c); g_runOpen = true; }
}

// ---- 서식 ------------------------------------------------------------------
static void ApplyFontAll(Node* n) {
    if (n->isPane) {
        if (n->edit) SendMessageW(n->edit, WM_SETFONT, (WPARAM)g_font, TRUE);
        return;
    }
    ApplyFontAll(n->a); ApplyFontAll(n->b);
}

// 자동 줄 바꿈은 EDIT 스타일로 바꿀 수 없어 컨트롤을 다시 만든다 (메모장도 동일).
static void RecreateEdits(Node* n) {
    if (n->isPane) {
        if (!n->edit) return;
        std::wstring s = PaneText(n);
        int a, b; GetSel(n->edit, a, b);
        bool focused = (GetFocus() == n->edit);
        DestroyWindow(n->edit);
        n->edit = CreateEditCtrl();
        n->vsb = n->hsb = true;        // 새 컨트롤은 스크롤바가 보이는 상태로 태어난다
        SetWindowTextW(n->edit, s.c_str());
        SendMessageW(n->edit, EM_SETSEL, a, b);
        if (focused) SetFocus(n->edit);
        return;
    }
    RecreateEdits(n->a); RecreateEdits(n->b);
}

static void ToggleWrap() {
    g_wrap = !g_wrap;
    g_quiet = true;
    RecreateEdits(g_root);
    g_quiet = false;
    Relayout();
    CheckMenuItem(GetMenu(g_main), IDM_WORDWRAP,
                  MF_BYCOMMAND | (g_wrap ? MF_CHECKED : MF_UNCHECKED));
}

static int ScreenDpiY() {
    HDC dc = GetDC(NULL);
    int d = GetDeviceCaps(dc, LOGPIXELSY);
    ReleaseDC(NULL, dc);
    return d;
}

// g_baseFont + g_basePt + g_zoom → g_font. 확대 배율은 창 전체에 적용된다.
static void ApplyFont() {
    LOGFONTW lf = g_baseFont;
    int px = MulDiv(MulDiv(g_basePt, ScreenDpiY(), 72), g_zoom, 100);
    lf.lfHeight = -(px < 1 ? 1 : px);
    HFONT nf = CreateFontIndirectW(&lf);
    if (!nf) return;
    HFONT old = g_font;
    g_font = nf;

    HDC dc = GetDC(NULL);                  // 한 줄 높이를 재둔다
    HFONT prev = (HFONT)SelectObject(dc, g_font);
    TEXTMETRICW tm;
    GetTextMetricsW(dc, &tm);
    SelectObject(dc, prev);
    ReleaseDC(NULL, dc);
    if (tm.tmHeight > 0) g_lineH = tm.tmHeight;

    if (g_root) ApplyFontAll(g_root);
    if (old) DeleteObject(old);
    Relayout();
    UpdateStatus();
}

static void SetZoom(int pct) {
    if (pct < 10) pct = 10;
    if (pct > 500) pct = 500;
    if (pct == g_zoom) return;
    g_zoom = pct;
    ApplyFont();
}

static void ChooseFontDlg() {
    LOGFONTW lf = g_baseFont;
    lf.lfHeight = -MulDiv(g_basePt, ScreenDpiY(), 72);
    CHOOSEFONTW cf = {};
    cf.lStructSize = sizeof(cf);
    cf.hwndOwner = g_main;
    cf.lpLogFont = &lf;
    cf.iPointSize = g_basePt * 10;
    cf.Flags = CF_INITTOLOGFONTSTRUCT | CF_SCREENFONTS | CF_NOVERTFONTS;
    if (!ChooseFontW(&cf)) return;
    g_baseFont = lf;
    g_basePt = cf.iPointSize / 10;
    if (g_basePt < 1) g_basePt = 1;
    ApplyFont();
}

static void InsertTimeDate() {
    if (!g_focus || !g_focus->edit) return;
    wchar_t t[64] = L"", d[64] = L"", out[160];
    GetTimeFormatW(LOCALE_USER_DEFAULT, TIME_NOSECONDS, NULL, NULL, t, 64);
    GetDateFormatW(LOCALE_USER_DEFAULT, DATE_SHORTDATE, NULL, NULL, d, 64);
    swprintf_s(out, L"%s %s", t, d);
    SendMessageW(g_focus->edit, EM_REPLACESEL, TRUE, (LPARAM)out);
}

// ---- 인코딩 ----------------------------------------------------------------
static std::string EncodeText(const std::wstring& w, int enc) {
    if (enc == ENC_UTF16LE) {
        std::string out("\xFF\xFE", 2);
        out.append((const char*)w.data(), w.size() * sizeof(wchar_t));
        return out;
    }
    UINT cp = (enc == ENC_ANSI) ? CP_ACP : CP_UTF8;
    int n = WideCharToMultiByte(cp, 0, w.data(), (int)w.size(), NULL, 0, NULL, NULL);
    std::string out(enc == ENC_UTF8BOM ? "\xEF\xBB\xBF" : "");
    size_t off = out.size();
    out.resize(off + (size_t)n);
    if (n) WideCharToMultiByte(cp, 0, w.data(), (int)w.size(), &out[off], n, NULL, NULL);
    return out;
}

// BOM이 있으면 그에 따르고, 없으면 현재 설정으로 해석한다 (SPEC §6.3).
static std::wstring DecodeBytes(const std::string& b, int& encOut) {
    const unsigned char* p = (const unsigned char*)b.data();
    size_t n = b.size();
    if (n >= 2 && p[0] == 0xFF && p[1] == 0xFE) {
        encOut = ENC_UTF16LE;
        return std::wstring((const wchar_t*)(p + 2), (n - 2) / sizeof(wchar_t));
    }
    size_t off = 0;
    if (n >= 3 && p[0] == 0xEF && p[1] == 0xBB && p[2] == 0xBF) {
        encOut = ENC_UTF8BOM; off = 3;
    } else if (encOut != ENC_ANSI) {
        encOut = ENC_UTF8;
    }
    UINT cp = (encOut == ENC_ANSI) ? CP_ACP : CP_UTF8;
    int cnt = MultiByteToWideChar(cp, 0, b.data() + off, (int)(n - off), NULL, 0);
    std::wstring w((size_t)cnt, L'\0');
    if (cnt) MultiByteToWideChar(cp, 0, b.data() + off, (int)(n - off), &w[0], cnt);
    return w;
}

static bool ReadWholeFile(const std::wstring& path, std::string& out) {
    HANDLE f = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, NULL,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (f == INVALID_HANDLE_VALUE) return false;
    LARGE_INTEGER sz;
    if (!GetFileSizeEx(f, &sz) || sz.QuadPart > 0x7FFFFFFF) { CloseHandle(f); return false; }
    out.resize((size_t)sz.QuadPart);
    DWORD got = 0;
    bool ok = out.empty() ||
              (ReadFile(f, &out[0], (DWORD)out.size(), &got, NULL) && got == out.size());
    CloseHandle(f);
    return ok;
}

static bool WriteWholeFile(const std::wstring& path, const std::string& data) {
    HANDLE f = CreateFileW(path.c_str(), GENERIC_WRITE, 0, NULL,
                           CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (f == INVALID_HANDLE_VALUE) return false;
    DWORD put = 0;
    bool ok = data.empty() ||
              (WriteFile(f, data.data(), (DWORD)data.size(), &put, NULL) && put == data.size());
    CloseHandle(f);
    return ok;
}

// ---- .etxt 직렬화 ----------------------------------------------------------
static void WriteNodeJson(Node* n, std::string& out, int& idx) {
    char buf[96];
    if (n->isPane) {
        sprintf_s(buf, "{\"p\":%d}", idx++);
        out += buf;
        return;
    }
    sprintf_s(buf, "{\"d\":\"%s\",\"r\":%.4f,\"a\":", n->dir == DIR_H ? "H" : "V", n->ratio);
    out += buf;
    WriteNodeJson(n->a, out, idx);
    out += ",\"b\":";
    WriteNodeJson(n->b, out, idx);
    out += "}";
}

static std::wstring BuildEtxt() {
    std::vector<Node*> leaves;
    CollectLeaves(g_root, leaves);
    int focus = 0;
    for (size_t i = 0; i < leaves.size(); ++i) if (leaves[i] == g_focus) focus = (int)i;

    std::string tree;
    int idx = 0;
    WriteNodeJson(g_root, tree, idx);

    char head[256];
    sprintf_s(head, "{\"root\":%s,\"focus\":%d,\"wrap\":%s}",
              tree.c_str(), focus, g_wrap ? "true" : "false");

    std::wstring w = L"ETXT1\n";
    for (const char* p = head; *p; ++p) w += (wchar_t)(unsigned char)*p;
    w += L'\n';
    for (size_t i = 0; i < leaves.size(); ++i) {
        wchar_t mark[32];
        swprintf_s(mark, L"PANE %d\n", (int)i);
        w += L'\0';
        w += mark;
        w += PaneText(leaves[i]);
    }
    return w;
}

// 이 포맷 전용 최소 JSON 파서.
// ponytail: 범용 JSON은 필요 없다. 키가 늘면 그때 확장.
struct Jp {
    const char* p;
    void ws() { while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') ++p; }
    bool lit(char c) { ws(); if (*p == c) { ++p; return true; } return false; }
    bool key(std::string& k) {
        ws();
        if (*p != '"') return false;
        ++p; k.clear();
        while (*p && *p != '"') k += *p++;
        if (*p == '"') ++p;
        ws();
        if (*p != ':') return false;
        ++p; return true;
    }
    std::string str() {
        ws(); std::string s;
        if (*p == '"') { ++p; while (*p && *p != '"') s += *p++; if (*p == '"') ++p; }
        return s;
    }
    double num() { ws(); char* e = NULL; double d = strtod(p, &e); if (e) p = e; return d; }
    bool word(const char* w) {
        ws(); size_t n = strlen(w);
        if (strncmp(p, w, n) == 0) { p += n; return true; }
        return false;
    }
};

static Node* ParseNodeJson(Jp& j, const std::vector<std::wstring>& bodies, bool& ok) {
    if (!j.lit('{')) { ok = false; return NULL; }
    int paneIdx = -1, dir = DIR_V;
    float ratio = 0.5f;
    Node* a = NULL; Node* b = NULL;
    std::string k;
    while (j.key(k)) {
        if      (k == "p") paneIdx = (int)j.num();
        else if (k == "d") dir = (j.str() == "H") ? DIR_H : DIR_V;
        else if (k == "r") ratio = (float)j.num();
        else if (k == "a") a = ParseNodeJson(j, bodies, ok);
        else if (k == "b") b = ParseNodeJson(j, bodies, ok);
        else ok = false;
        if (!ok) { FreeSubtree(a); FreeSubtree(b); return NULL; }
        if (!j.lit(',')) break;
    }
    if (!j.lit('}')) { ok = false; FreeSubtree(a); FreeSubtree(b); return NULL; }
    if (paneIdx >= 0) {
        Node* n = NewPane();
        if (n->edit && paneIdx < (int)bodies.size())
            SetWindowTextW(n->edit, bodies[(size_t)paneIdx].c_str());
        return n;
    }
    if (!a || !b) { ok = false; FreeSubtree(a); FreeSubtree(b); return NULL; }
    Node* s = NewSplit(dir);
    s->ratio = ratio;
    s->a = a; s->b = b;
    a->parent = s; b->parent = s;
    return s;
}

// 본문은 "\0PANE <n>\n" 구분자로 나뉜다. 본문 자체는 escape가 없어 그대로 잘라낸다.
static std::vector<std::wstring> SplitEtxtBodies(const std::wstring& w, size_t from) {
    std::vector<std::wstring> bodies;
    size_t at = w.find(L'\0', from);
    while (at != std::wstring::npos) {
        size_t nl = w.find(L'\n', at);
        if (nl == std::wstring::npos) break;
        size_t next = w.find(L'\0', nl + 1);
        size_t end = (next == std::wstring::npos) ? w.size() : next;
        bodies.push_back(w.substr(nl + 1, end - (nl + 1)));
        at = next;
    }
    return bodies;
}

// 성공하면 새 트리를 반환하고 focusIdx/wrap을 채운다.
static Node* ParseEtxt(const std::wstring& w, int& focusIdx, bool& wrap) {
    if (w.compare(0, 6, L"ETXT1\n") != 0) return NULL;
    size_t hs = 6;
    size_t he = w.find(L'\n', hs);
    if (he == std::wstring::npos) return NULL;
    std::string head;
    for (size_t i = hs; i < he; ++i)
        head += (w[i] < 128) ? (char)w[i] : '?';

    std::vector<std::wstring> bodies = SplitEtxtBodies(w, he);

    Jp j; j.p = head.c_str();
    if (!j.lit('{')) return NULL;
    Node* root = NULL;
    bool ok = true;
    focusIdx = 0; wrap = true;
    std::string k;
    while (j.key(k)) {
        if      (k == "root")  root = ParseNodeJson(j, bodies, ok);
        else if (k == "focus") focusIdx = (int)j.num();
        else if (k == "wrap")  wrap = j.word("true") ? true : (j.word("false") ? false : true);
        else ok = false;
        if (!ok) { FreeSubtree(root); return NULL; }
        if (!j.lit(',')) break;
    }
    return root;
}

// ---- 파일 명령 -------------------------------------------------------------
static bool HasExt(const std::wstring& p, const wchar_t* ext) {
    size_t n = wcslen(ext);
    if (p.size() < n) return false;
    return _wcsicmp(p.c_str() + p.size() - n, ext) == 0;
}

static void ReplaceTree(Node* newRoot, int focusIdx) {
    FreeSubtree(g_root);
    g_root = newRoot;
    g_root->parent = NULL;
    std::vector<Node*> leaves;
    CollectLeaves(g_root, leaves);
    if (focusIdx < 0 || focusIdx >= (int)leaves.size()) focusIdx = 0;
    g_focus = leaves[(size_t)focusIdx];
    Relayout();
    if (g_focus->edit) SetFocus(g_focus->edit);
}

static bool LoadFile(const std::wstring& path) {
    std::string bytes;
    if (!ReadWholeFile(path, bytes)) {
        MessageBoxW(g_main, L"파일을 열 수 없습니다.", L"더 나은 메모장",
                    MB_OK | MB_ICONWARNING);
        return false;
    }
    int enc = g_enc;
    std::wstring w = DecodeBytes(bytes, enc);

    g_quiet = true;
    int focusIdx = 0;
    bool wrap = g_wrap;
    Node* root = ParseEtxt(w, focusIdx, wrap);
    if (!root) {                       // SPEC §6.2: .txt는 단일 Pane
        root = NewPane();
        if (root->edit) SetWindowTextW(root->edit, w.c_str());
    } else if (wrap != g_wrap) {
        g_wrap = wrap;
        CheckMenuItem(GetMenu(g_main), IDM_WORDWRAP,
                      MF_BYCOMMAND | (g_wrap ? MF_CHECKED : MF_UNCHECKED));
    }
    ReplaceTree(root, focusIdx);
    g_quiet = false;

    g_enc = enc;
    CheckMenuRadioItem(GetMenu(g_main), IDM_ENC_FIRST, IDM_ENC_LAST,
                       IDM_ENC_FIRST + g_enc, MF_BYCOMMAND);
    g_path = path;
    ResetUndo();
    return true;
}

static bool SaveTo(const std::wstring& path) {
    std::wstring payload;
    if (HasExt(path, L".etxt")) {
        payload = BuildEtxt();
    } else {
        std::vector<Node*> leaves;
        CollectLeaves(g_root, leaves);
        for (size_t i = 0; i < leaves.size(); ++i) {
            if (i) payload += L"\r\n";
            payload += PaneText(leaves[i]);
        }
    }
    if (!WriteWholeFile(path, EncodeText(payload, g_enc))) {
        MessageBoxW(g_main, L"파일을 저장할 수 없습니다.", L"더 나은 메모장",
                    MB_OK | MB_ICONWARNING);
        return false;
    }
    return true;
}

static bool SaveAsDlg() {
    std::vector<Node*> leaves;
    CollectLeaves(g_root, leaves);

    wchar_t file[MAX_PATH] = L"";
    if (!g_path.empty()) wcscpy_s(file, BaseName(g_path));
    OPENFILENAMEW ofn = {};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = g_main;
    ofn.lpstrFilter = L"더 나은 메모장 (*.etxt)\0*.etxt\0텍스트 문서 (*.txt)\0*.txt\0모든 파일\0*.*\0";
    ofn.lpstrFile = file;
    ofn.nMaxFile = MAX_PATH;
    ofn.lpstrDefExt = L"etxt";
    ofn.Flags = OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST;
    if (!GetSaveFileNameW(&ofn)) return false;

    // SPEC §6.2: .txt로 저장하면 레이아웃이 사라진다
    if (!HasExt(file, L".etxt") && leaves.size() > 1 &&
        MessageBoxW(g_main,
                    L".txt로 저장하면 입력 공간의 배치가 사라지고 내용만 이어 붙여집니다.\n계속하시겠습니까?",
                    L"더 나은 메모장", MB_OKCANCEL | MB_ICONWARNING) != IDOK)
        return false;

    if (!SaveTo(file)) return false;
    g_path = file;
    g_savePoint = (long long)g_pos;
    UpdateDirty();
    UpdateTitle();
    return true;
}

static bool SaveDoc() {
    if (g_path.empty()) return SaveAsDlg();
    if (!SaveTo(g_path)) return false;
    g_savePoint = (long long)g_pos;
    UpdateDirty();
    return true;
}

// SPEC §4: 순수 export — 제목도 수정 상태도 바뀌지 않는다.
static void SavePaneOnly() {
    if (!g_focus || !g_focus->edit) return;
    wchar_t file[MAX_PATH] = L"";
    OPENFILENAMEW ofn = {};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = g_main;
    ofn.lpstrFilter = L"텍스트 문서 (*.txt)\0*.txt\0모든 파일\0*.*\0";
    ofn.lpstrFile = file;
    ofn.nMaxFile = MAX_PATH;
    ofn.lpstrDefExt = L"txt";
    ofn.Flags = OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST;
    if (!GetSaveFileNameW(&ofn)) return;
    WriteWholeFile(file, EncodeText(PaneText(g_focus), g_enc));
}

static bool ConfirmSave() {
    if (!g_dirty) return true;
    std::wstring name = g_path.empty() ? L"제목 없음" : BaseName(g_path);
    std::wstring msg = L"변경 내용을 " + name + L"에 저장하시겠습니까?";
    int r = MessageBoxW(g_main, msg.c_str(), L"더 나은 메모장",
                        MB_YESNOCANCEL | MB_ICONWARNING);
    if (r == IDCANCEL) return false;
    if (r == IDYES)    return SaveDoc();
    return true;
}

static void NewDoc() {
    if (!ConfirmSave()) return;
    g_quiet = true;
    ReplaceTree(NewPane(), 0);
    g_quiet = false;
    g_path.clear();
    ResetUndo();
}

static void NewWindow() {
    wchar_t exe[MAX_PATH];
    GetModuleFileNameW(NULL, exe, MAX_PATH);
    STARTUPINFOW si = { sizeof(si) };
    PROCESS_INFORMATION pi = {};
    if (CreateProcessW(exe, NULL, NULL, NULL, FALSE, 0, NULL, NULL, &si, &pi)) {
        CloseHandle(pi.hProcess);
        CloseHandle(pi.hThread);
    }
}

static void OpenDoc() {
    if (!ConfirmSave()) return;
    wchar_t file[MAX_PATH] = L"";
    OPENFILENAMEW ofn = {};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = g_main;
    ofn.lpstrFilter = L"메모장 문서 (*.etxt;*.txt)\0*.etxt;*.txt\0모든 파일\0*.*\0";
    ofn.lpstrFile = file;
    ofn.nMaxFile = MAX_PATH;
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST;
    if (GetOpenFileNameW(&ofn)) LoadFile(file);
}

// ---- 상태 표시줄 -----------------------------------------------------------
static void UpdateStatus() {
    if (!g_status || !g_statusOn) return;
    wchar_t buf[160];
    if (g_focus && g_focus->edit) {
        int a, b; GetSel(g_focus->edit, a, b);
        int line = (int)SendMessageW(g_focus->edit, EM_LINEFROMCHAR, a, 0);
        int idx  = (int)SendMessageW(g_focus->edit, EM_LINEINDEX, line, 0);
        swprintf_s(buf, L" 줄 %d, 열 %d", line + 1, a - idx + 1);
    } else {
        wcscpy_s(buf, L"");
    }
    SendMessageW(g_status, SB_SETTEXTW, 0, (LPARAM)buf);
    swprintf_s(buf, L" %d%%", g_zoom);
    SendMessageW(g_status, SB_SETTEXTW, 1, (LPARAM)buf);
    SendMessageW(g_status, SB_SETTEXTW, 2, (LPARAM)L" Windows (CRLF)");
    swprintf_s(buf, L" %s", kEncNames[g_enc]);
    SendMessageW(g_status, SB_SETTEXTW, 3, (LPARAM)buf);
}

static void LayoutStatus() {
    if (!g_status) return;
    RECT rc; GetClientRect(g_main, &rc);
    int parts[4] = { rc.right - 290, rc.right - 190, rc.right - 100, -1 };
    for (int i = 0; i < 3; ++i) {              // 좁은 창에서 순서가 뒤집히지 않게
        if (parts[i] < 0) parts[i] = 0;
        if (i && parts[i] < parts[i - 1]) parts[i] = parts[i - 1];
    }
    SendMessageW(g_status, SB_SETPARTS, 4, (LPARAM)parts);
    UpdateStatus();
}

static void ToggleStatusBar() {
    g_statusOn = !g_statusOn;
    ShowWindow(g_status, g_statusOn ? SW_SHOW : SW_HIDE);
    CheckMenuItem(GetMenu(g_main), IDM_STATUSBAR,
                  MF_BYCOMMAND | (g_statusOn ? MF_CHECKED : MF_UNCHECKED));
    Relayout();
    LayoutStatus();
}

// ---- 찾기 핵심 (순수 함수로 두어 검증한다) ---------------------------------
// ponytail: CharLowerBuffW는 길이를 바꾸지 않아 인덱스가 그대로 쓸 수 있다.
// 길이가 바뀌는 케이스 폴딩이 필요해지면 인덱스 매핑을 따로 만들어야 한다.
static void FoldCase(std::wstring& s) {
    if (!s.empty()) CharLowerBuffW(&s[0], (DWORD)s.size());
}

static int FindInText(const std::wstring& hay, const std::wstring& needle,
                      int from, bool up, bool matchCase) {
    if (needle.empty() || needle.size() > hay.size()) return -1;
    std::wstring h = hay, n = needle;
    if (!matchCase) { FoldCase(h); FoldCase(n); }
    if (from < 0) from = 0;
    if (up) {
        if (from <= 0) return -1;
        size_t r = h.rfind(n, (size_t)(from - 1));
        return r == std::wstring::npos ? -1 : (int)r;
    }
    if ((size_t)from > h.size()) return -1;
    size_t r = h.find(n, (size_t)from);
    return r == std::wstring::npos ? -1 : (int)r;
}

static int ReplaceAllInText(std::wstring& s, const std::wstring& from,
                            const std::wstring& to, bool matchCase) {
    if (from.empty()) return 0;
    std::wstring h = s, n = from;
    if (!matchCase) { FoldCase(h); FoldCase(n); }
    std::wstring out;
    out.reserve(s.size());
    size_t i = 0;
    int cnt = 0;
    for (;;) {
        size_t p = h.find(n, i);
        if (p == std::wstring::npos) { out.append(s, i, std::wstring::npos); break; }
        out.append(s, i, p - i);
        out += to;
        i = p + n.size();
        ++cnt;
    }
    if (cnt) s.swap(out);
    return cnt;
}

// ---- 찾기 / 바꾸기 ---------------------------------------------------------
static void SelectHit(Node* n, int pos, int len) {
    g_focus = n;
    SetFocus(n->edit);
    SendMessageW(n->edit, EM_SETSEL, pos, pos + len);
    SendMessageW(n->edit, EM_SCROLLCARET, 0, 0);
    BreakRun();
    UpdateStatus();
}

// ponytail: Pane마다 텍스트를 한 번 복사한다. 찾기는 사용자가 누를 때만 도니
// 문서가 커도 체감되지 않는다. 문제가 되면 범위 읽기로 바꾼다.
static bool FindPass(const std::vector<Node*>& v, int start, int count,
                     int firstFrom, bool useFirstFrom) {
    int n = (int)v.size();
    for (int k = 0; k < count; ++k) {
        int pi = (count == 1) ? start
                              : ((start + (g_find.up ? -k : k)) % n + n) % n;
        Node* p = v[(size_t)pi];
        if (!p->edit) continue;
        std::wstring t = PaneText(p);
        int f = (k == 0 && useFirstFrom) ? firstFrom
                                        : (g_find.up ? (int)t.size() : 0);
        int hit = FindInText(t, g_find.what, f, g_find.up, g_find.matchCase);
        if (hit >= 0) { SelectHit(p, hit, (int)g_find.what.size()); return true; }
    }
    return false;
}

static bool FindNext(bool report) {
    if (g_find.what.empty() || !g_focus || !g_focus->edit) return false;
    std::vector<Node*> v;
    CollectLeaves(g_root, v);
    int start = 0;
    for (size_t i = 0; i < v.size(); ++i) if (v[i] == g_focus) start = (int)i;

    int sa, sb; GetSel(g_focus->edit, sa, sb);
    int count = g_find.allPanes ? (int)v.size() : 1;

    if (FindPass(v, start, count, g_find.up ? sa : sb, true)) return true;
    if (g_find.wrapAround && FindPass(v, start, count, 0, false)) return true;

    if (report) {
        std::wstring m = L"\"" + g_find.what + L"\"을(를) 찾을 수 없습니다.";
        MessageBoxW(g_findDlg ? g_findDlg : g_main, m.c_str(), L"더 나은 메모장",
                    MB_OK | MB_ICONINFORMATION);
    }
    return false;
}

static void ReplaceOne() {
    if (!g_focus || !g_focus->edit || g_find.what.empty()) return;
    BreakRun();
    int a, b; GetSel(g_focus->edit, a, b);
    if (a != b) {
        std::wstring sel = EditRange(g_focus->edit, a, b), what = g_find.what;
        if (!g_find.matchCase) { FoldCase(sel); FoldCase(what); }
        if (sel == what)
            SendMessageW(g_focus->edit, EM_REPLACESEL, TRUE, (LPARAM)g_find.with.c_str());
    }
    FindNext(true);
}

// Pane마다 한 번의 치환으로 끝낸다 → undo도 Pane당 한 단계.
static void ReplaceAll() {
    if (g_find.what.empty()) return;
    BreakRun();
    std::vector<Node*> v;
    CollectLeaves(g_root, v);
    int total = 0;
    for (size_t i = 0; i < v.size(); ++i) {
        Node* p = v[i];
        if (!p->edit) continue;
        if (!g_find.allPanes && p != g_focus) continue;
        std::wstring t = PaneText(p);
        int c = ReplaceAllInText(t, g_find.what, g_find.with, g_find.matchCase);
        if (!c) continue;
        SendMessageW(p->edit, EM_SETSEL, 0, -1);
        SendMessageW(p->edit, EM_REPLACESEL, TRUE, (LPARAM)t.c_str());
        total += c;
    }
    wchar_t msg[128];
    if (total) swprintf_s(msg, L"%d개를 바꿨습니다.", total);
    else       swprintf_s(msg, L"찾을 내용이 없습니다.");
    MessageBoxW(g_findDlg ? g_findDlg : g_main, msg, L"더 나은 메모장",
                MB_OK | MB_ICONINFORMATION);
    UpdateStatus();
}

static void ReadFindDlg(HWND d) {
    wchar_t buf[512];
    GetDlgItemTextW(d, IDC_FIND_WHAT, buf, 512);
    g_find.what = buf;
    if (g_findIsReplace) {
        GetDlgItemTextW(d, IDC_REPL_WITH, buf, 512);
        g_find.with = buf;
    }
    g_find.matchCase  = IsDlgButtonChecked(d, IDC_MATCHCASE)  == BST_CHECKED;
    g_find.wrapAround = IsDlgButtonChecked(d, IDC_WRAPAROUND)  == BST_CHECKED;
    g_find.allPanes   = IsDlgButtonChecked(d, IDC_ALLPANES)    == BST_CHECKED;
    g_find.up = g_findIsReplace ? false
                                : (IsDlgButtonChecked(d, IDC_DIR_UP) == BST_CHECKED);
}

static INT_PTR CALLBACK FindProc(HWND d, UINT m, WPARAM w, LPARAM) {
    switch (m) {
    case WM_INITDIALOG:
        SetDlgItemTextW(d, IDC_FIND_WHAT, g_find.what.c_str());
        if (g_findIsReplace) SetDlgItemTextW(d, IDC_REPL_WITH, g_find.with.c_str());
        CheckDlgButton(d, IDC_MATCHCASE,  g_find.matchCase  ? BST_CHECKED : BST_UNCHECKED);
        CheckDlgButton(d, IDC_WRAPAROUND, g_find.wrapAround ? BST_CHECKED : BST_UNCHECKED);
        CheckDlgButton(d, IDC_ALLPANES,   g_find.allPanes   ? BST_CHECKED : BST_UNCHECKED);
        if (!g_findIsReplace)
            CheckRadioButton(d, IDC_DIR_UP, IDC_DIR_DOWN,
                             g_find.up ? IDC_DIR_UP : IDC_DIR_DOWN);
        return TRUE;
    case WM_COMMAND:
        switch (LOWORD(w)) {
        case IDC_FINDNEXT:   ReadFindDlg(d); FindNext(true); return TRUE;
        case IDC_REPLACE1:   ReadFindDlg(d); ReplaceOne();   return TRUE;
        case IDC_REPLACEALL: ReadFindDlg(d); ReplaceAll();   return TRUE;
        case IDCANCEL:
            ReadFindDlg(d);
            DestroyWindow(d);
            g_findDlg = NULL;
            return TRUE;
        }
        break;
    case WM_CLOSE:
        DestroyWindow(d);
        g_findDlg = NULL;
        return TRUE;
    }
    return FALSE;
}

static void ShowFindDlg(bool replace) {
    if (g_findDlg) { DestroyWindow(g_findDlg); g_findDlg = NULL; }
    // 선택된 글자가 있으면 찾을 내용으로 가져온다 (메모장과 동일)
    if (g_focus && g_focus->edit) {
        int a, b; GetSel(g_focus->edit, a, b);
        if (a != b && b - a < 256) g_find.what = EditRange(g_focus->edit, a, b);
    }
    g_findIsReplace = replace;
    g_findDlg = CreateDialogParamW(GetModuleHandleW(NULL),
                                   MAKEINTRESOURCEW(replace ? IDD_REPLACE : IDD_FIND),
                                   g_main, FindProc, 0);
    if (g_findDlg) ShowWindow(g_findDlg, SW_SHOW);
}

// ---- 줄 이동 ---------------------------------------------------------------
static INT_PTR CALLBACK GotoProc(HWND d, UINT m, WPARAM w, LPARAM) {
    if (m == WM_INITDIALOG) {
        int line = 1;
        if (g_focus && g_focus->edit) {
            int a, b; GetSel(g_focus->edit, a, b);
            line = (int)SendMessageW(g_focus->edit, EM_LINEFROMCHAR, a, 0) + 1;
        }
        SetDlgItemInt(d, IDC_GOTO_LINE, (UINT)line, FALSE);
        return TRUE;
    }
    if (m == WM_COMMAND) {
        if (LOWORD(w) == IDOK) {
            BOOL ok = FALSE;
            UINT n = GetDlgItemInt(d, IDC_GOTO_LINE, &ok, FALSE);
            if (ok && n >= 1 && g_focus && g_focus->edit) {
                HWND e = g_focus->edit;
                int cnt = (int)SendMessageW(e, EM_GETLINECOUNT, 0, 0);
                int line = (int)n - 1;
                if (line >= cnt) line = cnt - 1;
                int idx = (int)SendMessageW(e, EM_LINEINDEX, line, 0);
                if (idx >= 0) {
                    SetFocus(e);
                    SendMessageW(e, EM_SETSEL, idx, idx);
                    SendMessageW(e, EM_SCROLLCARET, 0, 0);
                    BreakRun();
                    UpdateStatus();
                }
            }
            EndDialog(d, IDOK);
            return TRUE;
        }
        if (LOWORD(w) == IDCANCEL) { EndDialog(d, IDCANCEL); return TRUE; }
    }
    return FALSE;
}

// ---- Bing으로 검색 ---------------------------------------------------------
static void BingSearch() {
    if (!g_focus || !g_focus->edit) return;
    int a, b; GetSel(g_focus->edit, a, b);
    if (a == b) return;
    std::wstring sel = EditRange(g_focus->edit, a, b);
    if (sel.size() > 200) sel.resize(200);
    std::string u8 = EncodeText(sel, ENC_UTF8);
    std::wstring url = L"https://www.bing.com/search?q=";
    for (size_t i = 0; i < u8.size(); ++i) {
        unsigned char c = (unsigned char)u8[i];
        bool safe = (c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z') ||
                    (c >= 'a' && c <= 'z') || c == '-' || c == '_' ||
                    c == '.' || c == '~';
        if (safe) url += (wchar_t)c;
        else { wchar_t h[8]; swprintf_s(h, L"%%%02X", c); url += h; }
    }
    ShellExecuteW(g_main, L"open", url.c_str(), NULL, NULL, SW_SHOWNORMAL);
}

// ---- 인쇄 ------------------------------------------------------------------
static void InitPageSetup() {
    if (g_psdInit) return;
    ZeroMemory(&g_psd, sizeof(g_psd));
    g_psd.lStructSize = sizeof(g_psd);
    g_psd.Flags = PSD_INTHOUSANDTHSOFINCHES | PSD_MARGINS;
    g_psd.rtMargin.left = g_psd.rtMargin.right = 750;     // 0.75"
    g_psd.rtMargin.top  = g_psd.rtMargin.bottom = 1000;   // 1.0"
    g_psdInit = true;
}

static void PageSetup() {
    InitPageSetup();
    g_psd.hwndOwner = g_main;
    PageSetupDlgW(&g_psd);
}

static std::wstring DocumentText() {
    std::vector<Node*> v;
    CollectLeaves(g_root, v);
    std::wstring s;
    for (size_t i = 0; i < v.size(); ++i) {
        if (i) s += L"\r\n";
        s += PaneText(v[i]);
    }
    return s;
}

// 문서 전체를 순회 순서로 인쇄한다.
// ponytail: 머리글/바닥글은 없다. 한 줄이 한 페이지보다 높으면 잘린다.
static void PrintDoc() {
    InitPageSetup();
    PRINTDLGW pd = {};
    pd.lStructSize = sizeof(pd);
    pd.hwndOwner = g_main;
    pd.Flags = PD_RETURNDC | PD_NOSELECTION | PD_NOPAGENUMS;
    if (!PrintDlgW(&pd) || !pd.hDC) return;
    HDC dc = pd.hDC;

    int dpiX = GetDeviceCaps(dc, LOGPIXELSX), dpiY = GetDeviceCaps(dc, LOGPIXELSY);
    int pw = GetDeviceCaps(dc, HORZRES), ph = GetDeviceCaps(dc, VERTRES);
    int offX = GetDeviceCaps(dc, PHYSICALOFFSETX), offY = GetDeviceCaps(dc, PHYSICALOFFSETY);
    RECT m;
    m.left   = MulDiv(g_psd.rtMargin.left,   dpiX, 1000) - offX;
    m.top    = MulDiv(g_psd.rtMargin.top,    dpiY, 1000) - offY;
    m.right  = pw - (MulDiv(g_psd.rtMargin.right,  dpiX, 1000) - offX);
    m.bottom = ph - (MulDiv(g_psd.rtMargin.bottom, dpiY, 1000) - offY);
    if (m.left < 0) m.left = 0;
    if (m.top < 0) m.top = 0;
    if (m.right <= m.left) m.right = pw;
    if (m.bottom <= m.top) m.bottom = ph;

    LOGFONTW lf = g_baseFont;                      // 확대 배율은 화면만의 것
    lf.lfHeight = -MulDiv(g_basePt, dpiY, 72);
    HFONT pf = CreateFontIndirectW(&lf);
    HFONT of = (HFONT)SelectObject(dc, pf);

    std::wstring title = g_path.empty() ? L"제목 없음" : BaseName(g_path);
    DOCINFOW di = { sizeof(di) };
    di.lpszDocName = title.c_str();

    if (StartDocW(dc, &di) > 0) {
        std::wstring text = DocumentText();
        TEXTMETRICW tm; GetTextMetricsW(dc, &tm);
        const UINT fmt = DT_WORDBREAK | DT_NOPREFIX | DT_EXPANDTABS;
        int y = m.top;
        bool onPage = false;
        size_t i = 0;
        while (i <= text.size()) {
            size_t e = text.find(L"\r\n", i);
            size_t end = (e == std::wstring::npos) ? text.size() : e;
            std::wstring line = text.substr(i, end - i);
            i = (e == std::wstring::npos) ? text.size() + 1 : e + 2;

            int h = tm.tmHeight;
            if (!line.empty()) {
                RECT c = { m.left, 0, m.right, 0 };
                DrawTextW(dc, line.c_str(), (int)line.size(), &c, fmt | DT_CALCRECT);
                h = c.bottom;
            }
            if (!onPage) {
                if (StartPage(dc) <= 0) break;
                onPage = true;
                y = m.top;
            } else if (y > m.top && y + h > m.bottom) {
                EndPage(dc);
                if (StartPage(dc) <= 0) { onPage = false; break; }
                y = m.top;
            }
            if (!line.empty()) {
                RECT r = { m.left, y, m.right, m.bottom };
                DrawTextW(dc, line.c_str(), (int)line.size(), &r, fmt);
            }
            y += h;
        }
        if (onPage) EndPage(dc);
        EndDoc(dc);
    }

    SelectObject(dc, of);
    DeleteObject(pf);
    DeleteDC(dc);
    if (pd.hDevMode) GlobalFree(pd.hDevMode);
    if (pd.hDevNames) GlobalFree(pd.hDevNames);
}

// ---- 설정 ------------------------------------------------------------------
static std::wstring SettingsPath() {
    wchar_t buf[MAX_PATH];
    DWORD n = GetEnvironmentVariableW(L"APPDATA", buf, MAX_PATH);
    if (!n || n >= MAX_PATH) return L"";
    std::wstring dir = std::wstring(buf) + L"\\ExtendedNotepad";
    CreateDirectoryW(dir.c_str(), NULL);
    return dir + L"\\settings.ini";
}

static void LoadSettings() {
    ZeroMemory(&g_baseFont, sizeof(g_baseFont));
    g_baseFont.lfCharSet = DEFAULT_CHARSET;
    g_baseFont.lfWeight = FW_NORMAL;
    wcscpy_s(g_baseFont.lfFaceName, L"Consolas");
    g_basePt = 11;

    std::wstring p = SettingsPath();
    if (p.empty()) return;
    const wchar_t* f = p.c_str();
    int e = (int)GetPrivateProfileIntW(L"General", L"Encoding", ENC_UTF8, f);
    g_enc = (e >= ENC_UTF8 && e <= ENC_ANSI) ? e : ENC_UTF8;
    g_wrap     = GetPrivateProfileIntW(L"General", L"WordWrap", 1, f) != 0;
    g_statusOn = GetPrivateProfileIntW(L"General", L"StatusBar", 1, f) != 0;

    wchar_t face[LF_FACESIZE];
    GetPrivateProfileStringW(L"Font", L"Face", L"Consolas", face, LF_FACESIZE, f);
    wcscpy_s(g_baseFont.lfFaceName, face);
    int pt = (int)GetPrivateProfileIntW(L"Font", L"Point", 11, f);
    g_basePt = (pt >= 1 && pt <= 400) ? pt : 11;
    g_baseFont.lfWeight = (LONG)GetPrivateProfileIntW(L"Font", L"Weight", FW_NORMAL, f);
    g_baseFont.lfItalic = (BYTE)GetPrivateProfileIntW(L"Font", L"Italic", 0, f);

    int x = (int)GetPrivateProfileIntW(L"Window", L"X", 0, f);
    int y = (int)GetPrivateProfileIntW(L"Window", L"Y", 0, f);
    int cx = (int)GetPrivateProfileIntW(L"Window", L"W", 0, f);
    int cy = (int)GetPrivateProfileIntW(L"Window", L"H", 0, f);
    g_placeMax = GetPrivateProfileIntW(L"Window", L"Maximized", 0, f) != 0;
    if (cx >= 200 && cy >= 150) {
        SetRect(&g_place, x, y, x + cx, y + cy);
        g_hasPlace = true;
    }
}

// 모니터 구성이 바뀌어 저장된 자리가 화면 밖이면 쓰지 않는다.
static bool PlaceOnScreen(const RECT& r) {
    return MonitorFromRect(&r, MONITOR_DEFAULTTONULL) != NULL;
}

static void SaveWindowPlacement(const wchar_t* f) {
    if (!g_main) return;
    WINDOWPLACEMENT wp;
    ZeroMemory(&wp, sizeof(wp));
    wp.length = sizeof(wp);
    if (!GetWindowPlacement(g_main, &wp)) return;
    // rcNormalPosition은 최대화/최소화 중에도 "복원했을 때의 자리"를 담고 있다
    const RECT& r = wp.rcNormalPosition;
    wchar_t v[32];
    swprintf_s(v, L"%d", (int)r.left);
    WritePrivateProfileStringW(L"Window", L"X", v, f);
    swprintf_s(v, L"%d", (int)r.top);
    WritePrivateProfileStringW(L"Window", L"Y", v, f);
    swprintf_s(v, L"%d", (int)(r.right - r.left));
    WritePrivateProfileStringW(L"Window", L"W", v, f);
    swprintf_s(v, L"%d", (int)(r.bottom - r.top));
    WritePrivateProfileStringW(L"Window", L"H", v, f);
    swprintf_s(v, L"%d", wp.showCmd == SW_SHOWMAXIMIZED ? 1 : 0);
    WritePrivateProfileStringW(L"Window", L"Maximized", v, f);
}

static void SaveSettings() {
    std::wstring p = SettingsPath();
    if (p.empty()) return;
    const wchar_t* f = p.c_str();
    wchar_t v[32];
    swprintf_s(v, L"%d", g_enc);
    WritePrivateProfileStringW(L"General", L"Encoding", v, f);
    swprintf_s(v, L"%d", g_wrap ? 1 : 0);
    WritePrivateProfileStringW(L"General", L"WordWrap", v, f);
    swprintf_s(v, L"%d", g_statusOn ? 1 : 0);
    WritePrivateProfileStringW(L"General", L"StatusBar", v, f);
    WritePrivateProfileStringW(L"Font", L"Face", g_baseFont.lfFaceName, f);
    swprintf_s(v, L"%d", g_basePt);
    WritePrivateProfileStringW(L"Font", L"Point", v, f);
    swprintf_s(v, L"%d", (int)g_baseFont.lfWeight);
    WritePrivateProfileStringW(L"Font", L"Weight", v, f);
    swprintf_s(v, L"%d", (int)g_baseFont.lfItalic);
    WritePrivateProfileStringW(L"Font", L"Italic", v, f);
    SaveWindowPlacement(f);
}

static LOGFONTW g_dlgFont;
static int      g_dlgPt;

static void SetFontLabel(HWND d) {
    wchar_t s[160];
    swprintf_s(s, L"%s, %dpt", g_dlgFont.lfFaceName, g_dlgPt);
    SetDlgItemTextW(d, IDC_SET_FONTNAME, s);
}

static INT_PTR CALLBACK SettingsProc(HWND d, UINT m, WPARAM w, LPARAM) {
    switch (m) {
    case WM_INITDIALOG:
        for (int i = 0; i < 4; ++i)
            SendDlgItemMessageW(d, IDC_SET_ENC, CB_ADDSTRING, 0, (LPARAM)kEncNames[i]);
        SendDlgItemMessageW(d, IDC_SET_ENC, CB_SETCURSEL, (WPARAM)g_enc, 0);
        CheckDlgButton(d, IDC_SET_WRAP, g_wrap ? BST_CHECKED : BST_UNCHECKED);
        g_dlgFont = g_baseFont;
        g_dlgPt = g_basePt;
        SetFontLabel(d);
        return TRUE;
    case WM_COMMAND:
        switch (LOWORD(w)) {
        case IDC_SET_FONT: {
            LOGFONTW lf = g_dlgFont;
            lf.lfHeight = -MulDiv(g_dlgPt, ScreenDpiY(), 72);
            CHOOSEFONTW cf = {};
            cf.lStructSize = sizeof(cf);
            cf.hwndOwner = d;
            cf.lpLogFont = &lf;
            cf.iPointSize = g_dlgPt * 10;
            cf.Flags = CF_INITTOLOGFONTSTRUCT | CF_SCREENFONTS | CF_NOVERTFONTS;
            if (ChooseFontW(&cf)) {
                g_dlgFont = lf;
                g_dlgPt = cf.iPointSize / 10;
                if (g_dlgPt < 1) g_dlgPt = 1;
                SetFontLabel(d);
            }
            return TRUE;
        }
        case IDOK: {
            int e = (int)SendDlgItemMessageW(d, IDC_SET_ENC, CB_GETCURSEL, 0, 0);
            if (e >= ENC_UTF8 && e <= ENC_ANSI) {
                g_enc = e;
                CheckMenuRadioItem(GetMenu(g_main), IDM_ENC_FIRST, IDM_ENC_LAST,
                                   IDM_ENC_FIRST + g_enc, MF_BYCOMMAND);
            }
            bool wrap = IsDlgButtonChecked(d, IDC_SET_WRAP) == BST_CHECKED;
            g_baseFont = g_dlgFont;
            g_basePt = g_dlgPt;
            ApplyFont();
            if (wrap != g_wrap) ToggleWrap();
            UpdateStatus();
            SaveSettings();
            EndDialog(d, IDOK);
            return TRUE;
        }
        case IDCANCEL:
            EndDialog(d, IDCANCEL);
            return TRUE;
        }
        break;
    }
    return FALSE;
}

static void About() {
    MessageBoxW(g_main,
                L"더 나은 메모장\n\n"
                L"하나의 창에서 여러 입력 공간을 분할해 쓰는 메모장.\n\n"
                L"Ctrl+R  좌우 분할\n"
                L"Ctrl+T  상하 분할\n"
                L"Ctrl+W  현재 공간 닫기\n"
                L"Ctrl+Tab  다음 공간\n"
                L"Ctrl+Alt+S  현재 공간 단독 저장",
                L"더 나은 메모장 정보", MB_OK | MB_ICONINFORMATION);
}

// ---- 메뉴 ------------------------------------------------------------------
static void AppendPaneItems(HMENU m) {
    AppendMenuW(m, MF_STRING, IDM_SPLIT_V, L"좌우 분할(&R)\tCtrl+R");
    AppendMenuW(m, MF_STRING, IDM_SPLIT_H, L"상하 분할(&T)\tCtrl+T");
    AppendMenuW(m, MF_STRING, IDM_CLOSE_PANE, L"현재 공간 닫기(&W)\tCtrl+W");
    AppendMenuW(m, MF_STRING, IDM_SAVE_PANE, L"현재 공간 단독 저장\tCtrl+Alt+S");
}

static HMENU BuildMenuBar() {
    HMENU enc = CreateMenu();
    AppendMenuW(enc, MF_STRING, IDM_ENC_UTF8,    L"UTF-8");
    AppendMenuW(enc, MF_STRING, IDM_ENC_UTF8BOM, L"UTF-8 (BOM)");
    AppendMenuW(enc, MF_STRING, IDM_ENC_UTF16LE, L"UTF-16 LE");
    AppendMenuW(enc, MF_STRING, IDM_ENC_ANSI,    L"ANSI");

    HMENU file = CreateMenu();
    AppendMenuW(file, MF_STRING, IDM_NEW, L"새로 만들기(&N)\tCtrl+N");
    AppendMenuW(file, MF_STRING, IDM_NEWWIN, L"새 창(&W)\tCtrl+Shift+N");
    AppendMenuW(file, MF_STRING, IDM_OPEN, L"열기(&O)...\tCtrl+O");
    AppendMenuW(file, MF_STRING, IDM_SAVE, L"저장(&S)\tCtrl+S");
    AppendMenuW(file, MF_STRING, IDM_SAVEAS, L"다른 이름으로 저장(&A)...\tCtrl+Shift+S");
    AppendMenuW(file, MF_SEPARATOR, 0, NULL);
    AppendMenuW(file, MF_STRING, IDM_SAVE_PANE, L"현재 공간 단독 저장\tCtrl+Alt+S");
    AppendMenuW(file, MF_SEPARATOR, 0, NULL);
    AppendMenuW(file, MF_POPUP, (UINT_PTR)enc, L"인코딩(&E)");
    AppendMenuW(file, MF_SEPARATOR, 0, NULL);
    AppendMenuW(file, MF_STRING, IDM_PAGESETUP, L"페이지 설정(&U)...");
    AppendMenuW(file, MF_STRING, IDM_PRINT, L"인쇄(&P)...\tCtrl+P");
    AppendMenuW(file, MF_SEPARATOR, 0, NULL);
    AppendMenuW(file, MF_STRING, IDM_EXIT, L"끝내기(&X)");

    HMENU edit = CreateMenu();
    AppendMenuW(edit, MF_STRING, IDM_UNDO, L"실행 취소(&U)\tCtrl+Z");
    AppendMenuW(edit, MF_STRING, IDM_REDO, L"다시 실행(&R)\tCtrl+Shift+Z");
    AppendMenuW(edit, MF_SEPARATOR, 0, NULL);
    AppendMenuW(edit, MF_STRING, IDM_CUT, L"잘라내기(&T)\tCtrl+X");
    AppendMenuW(edit, MF_STRING, IDM_COPY, L"복사(&C)\tCtrl+C");
    AppendMenuW(edit, MF_STRING, IDM_PASTE, L"붙여넣기(&P)\tCtrl+V");
    AppendMenuW(edit, MF_STRING, IDM_DELETE, L"삭제(&L)\tDel");
    AppendMenuW(edit, MF_SEPARATOR, 0, NULL);
    AppendMenuW(edit, MF_STRING, IDM_BINGSEARCH, L"Bing으로 검색(&B)\tCtrl+E");
    AppendMenuW(edit, MF_STRING, IDM_FIND, L"찾기(&F)...\tCtrl+F");
    AppendMenuW(edit, MF_STRING, IDM_FINDNEXT, L"다음 찾기(&N)\tF3");
    AppendMenuW(edit, MF_STRING, IDM_FINDPREV, L"이전 찾기(&V)\tShift+F3");
    AppendMenuW(edit, MF_STRING, IDM_REPLACE, L"바꾸기(&R)...\tCtrl+H");
    AppendMenuW(edit, MF_STRING, IDM_GOTO, L"이동(&G)...\tCtrl+G");
    AppendMenuW(edit, MF_SEPARATOR, 0, NULL);
    AppendMenuW(edit, MF_STRING, IDM_SELECT_ALL, L"모두 선택(&A)\tCtrl+A");
    AppendMenuW(edit, MF_STRING, IDM_TIMEDATE, L"시간/날짜(&D)\tF5");

    HMENU fmt = CreateMenu();
    AppendMenuW(fmt, MF_STRING | MF_CHECKED, IDM_WORDWRAP, L"자동 줄 바꿈(&W)");
    AppendMenuW(fmt, MF_STRING, IDM_FONT, L"글꼴(&F)...");
    AppendMenuW(fmt, MF_SEPARATOR, 0, NULL);
    AppendMenuW(fmt, MF_STRING, IDM_SETTINGS, L"설정(&S)...");

    HMENU view = CreateMenu();
    AppendMenuW(view, MF_STRING, IDM_ZOOMIN, L"확대(&I)\tCtrl++");
    AppendMenuW(view, MF_STRING, IDM_ZOOMOUT, L"축소(&O)\tCtrl+-");
    AppendMenuW(view, MF_STRING, IDM_ZOOMRESET, L"기본 크기 복원(&R)\tCtrl+0");
    AppendMenuW(view, MF_SEPARATOR, 0, NULL);
    AppendMenuW(view, MF_STRING | MF_CHECKED, IDM_STATUSBAR, L"상태 표시줄(&S)");

    HMENU help = CreateMenu();
    AppendMenuW(help, MF_STRING, IDM_ABOUT, L"정보(&A)");

    HMENU pane = CreateMenu();
    AppendPaneItems(pane);
    AppendMenuW(pane, MF_SEPARATOR, 0, NULL);
    AppendMenuW(pane, MF_STRING, IDM_NEXT_PANE, L"다음 공간(&N)\tCtrl+Tab");
    AppendMenuW(pane, MF_STRING, IDM_PREV_PANE, L"이전 공간(&P)\tCtrl+Shift+Tab");

    HMENU bar = CreateMenu();
    AppendMenuW(bar, MF_POPUP, (UINT_PTR)file, L"파일(&F)");
    AppendMenuW(bar, MF_POPUP, (UINT_PTR)edit, L"편집(&E)");
    AppendMenuW(bar, MF_POPUP, (UINT_PTR)fmt,  L"서식(&O)");
    AppendMenuW(bar, MF_POPUP, (UINT_PTR)view, L"보기(&V)");
    AppendMenuW(bar, MF_POPUP, (UINT_PTR)pane, L"공간(&S)");
    AppendMenuW(bar, MF_POPUP, (UINT_PTR)help, L"도움말(&H)");
    return bar;
}

// SPEC §4: 기존 편집 항목 아래 구분선으로 분리하고 분할 항목 4개를 추가.
static void ShowPaneContextMenu(HWND edit, int x, int y) {
    int a, b; GetSel(edit, a, b);
    bool hasSel = (a != b);
    bool canPaste = IsClipboardFormatAvailable(CF_UNICODETEXT) != 0;
    UINT selFlag = hasSel ? MF_STRING : (MF_STRING | MF_GRAYED);

    HMENU m = CreatePopupMenu();
    AppendMenuW(m, MF_STRING | (g_pos > 0 ? 0 : MF_GRAYED), IDM_UNDO, L"실행 취소(&U)");
    AppendMenuW(m, MF_STRING | (g_pos < g_cmds.size() ? 0 : MF_GRAYED), IDM_REDO, L"다시 실행(&R)");
    AppendMenuW(m, MF_SEPARATOR, 0, NULL);
    AppendMenuW(m, selFlag, IDM_CUT, L"잘라내기(&T)");
    AppendMenuW(m, selFlag, IDM_COPY, L"복사(&C)");
    AppendMenuW(m, MF_STRING | (canPaste ? 0 : MF_GRAYED), IDM_PASTE, L"붙여넣기(&P)");
    AppendMenuW(m, selFlag, IDM_DELETE, L"삭제(&D)");
    AppendMenuW(m, MF_SEPARATOR, 0, NULL);
    AppendMenuW(m, MF_STRING, IDM_SELECT_ALL, L"모두 선택(&A)");
    AppendMenuW(m, MF_SEPARATOR, 0, NULL);
    AppendPaneItems(m);
    TrackPopupMenu(m, TPM_RIGHTBUTTON, x, y, 0, g_main, NULL);
    DestroyMenu(m);
}

// ---- EDIT 서브클래스 -------------------------------------------------------
static LRESULT CALLBACK EditProc(HWND h, UINT msg, WPARAM wp, LPARAM lp,
                                 UINT_PTR, DWORD_PTR) {
    if (msg == WM_CONTEXTMENU) {
        int x = GET_X_LPARAM(lp), y = GET_Y_LPARAM(lp);
        if (x == -1 && y == -1) {   // 키보드(Shift+F10)
            RECT rc; GetWindowRect(h, &rc);
            x = rc.left + 8; y = rc.top + 8;
        }
        Node* n = FindByHwnd(g_root, h);
        if (n) { g_focus = n; ShowPaneContextMenu(h, x, y); }
        return 0;
    }
    if (msg == WM_SETFOCUS) {
        Node* n = FindByHwnd(g_root, h);
        if (n && n != g_focus) { g_focus = n; BreakRun(); }
    }
    // Ctrl+휠로 확대/축소 (창 전체)
    if (msg == WM_MOUSEWHEEL && (GetKeyState(VK_CONTROL) & 0x8000)) {
        SetZoom(g_zoom + (GET_WHEEL_DELTA_WPARAM(wp) > 0 ? 10 : -10));
        return 0;
    }
    // 캐럿이 튀면 타이핑 묶음을 끊는다
    if (msg == WM_LBUTTONDOWN || (msg == WM_KEYDOWN && IsNavKey(wp))) BreakRun();

    if (!g_applying && !g_quiet && GateMsg(msg, wp)) {
        Node* n = FindByHwnd(g_root, h);
        if (n) {
            Pre pre = PreCapture(h);
            LRESULT r = DefSubclassProc(h, msg, wp, lp);
            PostCapture(h, n, pre);
            UpdateScrollbars(n);
            UpdateStatus();
            return r;
        }
    }
    LRESULT r = DefSubclassProc(h, msg, wp, lp);
    // 내용이 통째로 바뀌었으면 스크롤바를 다시 판정한다 (undo 기록 대상은 아니다)
    if (msg == WM_SETTEXT) {
        Node* n = FindByHwnd(g_root, h);
        if (n) UpdateScrollbars(n);
    }
    // 캐럿이 움직였을 수 있는 메시지 뒤에 줄/열을 갱신한다
    if (msg == WM_KEYDOWN || msg == WM_KEYUP || msg == WM_LBUTTONDOWN ||
        msg == WM_LBUTTONUP || msg == WM_MOUSEMOVE || msg == WM_SETFOCUS)
        UpdateStatus();
    return r;
}

// ---- 창 프로시저 -----------------------------------------------------------
static LRESULT CALLBACK WndProc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_SIZE:
        if (g_status) SendMessageW(g_status, WM_SIZE, 0, 0);
        Relayout();
        LayoutStatus();
        return 0;

    case WM_SETFOCUS:
        if (g_focus && g_focus->edit) SetFocus(g_focus->edit);
        return 0;

    case WM_LBUTTONDOWN: {
        POINT pt = { GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
        Node* s = HitSplitter(g_root, pt);
        if (s) { g_drag = s; SetCapture(h); }
        return 0;
    }

    case WM_MOUSEMOVE: {
        POINT pt = { GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
        if (g_drag) {
            RECT r = g_drag->rc;
            float t;
            if (g_drag->dir == DIR_V) {
                int avail = (r.right - r.left) - GAP;
                t = avail > 0 ? (pt.x - r.left - GAP * 0.5f) / avail : 0.5f;
            } else {
                int avail = (r.bottom - r.top) - GAP;
                t = avail > 0 ? (pt.y - r.top - GAP * 0.5f) / avail : 0.5f;
            }
            if (t < 0.f) t = 0.f;
            if (t > 1.f) t = 1.f;
            if (t == g_drag->ratio) return 0;      // 같은 픽셀이면 아무것도 하지 않는다
            g_drag->ratio = t;
            LayoutBatch(g_drag, g_drag->rc);
            // 자식이 비운 자리는 시스템이 무효화해 준다. 간격은 단색이라 다시
            // 그리지 않아도 차이가 없다 — 여기서 Invalidate하면 그게 깜빡임이 된다.
            return 0;
        }
        Node* s = HitSplitter(g_root, pt);
        SetCursor(LoadCursorW(NULL, !s ? IDC_ARROW
                              : (s->dir == DIR_V ? IDC_SIZEWE : IDC_SIZENS)));
        return 0;
    }

    case WM_LBUTTONUP:
        if (g_drag) { g_drag = NULL; ReleaseCapture(); }
        return 0;

    case WM_SETCURSOR: {
        POINT pt; GetCursorPos(&pt); ScreenToClient(h, &pt);
        if ((HWND)wp == h) {
            Node* s = g_drag ? g_drag : HitSplitter(g_root, pt);
            if (s) {
                SetCursor(LoadCursorW(NULL, s->dir == DIR_V ? IDC_SIZEWE : IDC_SIZENS));
                return TRUE;
            }
        }
        break;
    }

    case WM_INITMENUPOPUP: {
        HMENU m = (HMENU)wp;
        EnableMenuItem(m, IDM_UNDO, MF_BYCOMMAND | (g_pos > 0 ? MF_ENABLED : MF_GRAYED));
        EnableMenuItem(m, IDM_REDO, MF_BYCOMMAND |
                       (g_pos < g_cmds.size() ? MF_ENABLED : MF_GRAYED));
        // 메모장과 같이, 자동 줄 바꿈 중에는 줄 번호가 논리 줄과 달라 이동을 막는다
        EnableMenuItem(m, IDM_GOTO, MF_BYCOMMAND | (g_wrap ? MF_GRAYED : MF_ENABLED));
        EnableMenuItem(m, IDM_FINDNEXT, MF_BYCOMMAND |
                       (g_find.what.empty() ? MF_GRAYED : MF_ENABLED));
        EnableMenuItem(m, IDM_FINDPREV, MF_BYCOMMAND |
                       (g_find.what.empty() ? MF_GRAYED : MF_ENABLED));
        break;
    }

    case WM_COMMAND: {
        int id = LOWORD(wp);
        if (id >= IDM_ENC_FIRST && id <= IDM_ENC_LAST) {
            g_enc = id - IDM_ENC_FIRST;
            CheckMenuRadioItem(GetMenu(h), IDM_ENC_FIRST, IDM_ENC_LAST, id, MF_BYCOMMAND);
            return 0;
        }
        HWND e = (g_focus && g_focus->edit) ? g_focus->edit : NULL;
        switch (id) {
        case IDM_NEW:        NewDoc();           return 0;
        case IDM_NEWWIN:     NewWindow();        return 0;
        case IDM_OPEN:       OpenDoc();          return 0;
        case IDM_SAVE:       BreakRun(); SaveDoc();   return 0;
        case IDM_SAVEAS:     BreakRun(); SaveAsDlg(); return 0;
        case IDM_SAVE_PANE:  SavePaneOnly();     return 0;
        case IDM_PAGESETUP:  PageSetup();        return 0;
        case IDM_PRINT:      PrintDoc();         return 0;
        case IDM_EXIT:       PostMessageW(h, WM_CLOSE, 0, 0); return 0;
        case IDM_UNDO:       Undo();             return 0;
        case IDM_REDO:       Redo();             return 0;
        case IDM_SPLIT_V:    SplitFocus(DIR_V);  return 0;
        case IDM_SPLIT_H:    SplitFocus(DIR_H);  return 0;
        case IDM_CLOSE_PANE: CloseFocusPane();   return 0;
        case IDM_NEXT_PANE:  CyclePane(+1);      return 0;
        case IDM_PREV_PANE:  CyclePane(-1);      return 0;
        case IDM_CUT:        if (e) SendMessageW(e, WM_CUT, 0, 0); return 0;
        case IDM_COPY:       if (e) SendMessageW(e, WM_COPY, 0, 0); return 0;
        case IDM_PASTE:      if (e) SendMessageW(e, WM_PASTE, 0, 0); return 0;
        case IDM_DELETE:     if (e) SendMessageW(e, WM_CLEAR, 0, 0); return 0;
        case IDM_SELECT_ALL: if (e) { SetFocus(e); SendMessageW(e, EM_SETSEL, 0, -1); } return 0;
        case IDM_TIMEDATE:   InsertTimeDate();   return 0;
        case IDM_WORDWRAP:   ToggleWrap();       return 0;
        case IDM_FONT:       ChooseFontDlg();    return 0;
        case IDM_SETTINGS:
            DialogBoxParamW(GetModuleHandleW(NULL), MAKEINTRESOURCEW(IDD_SETTINGS),
                            h, SettingsProc, 0);
            return 0;
        case IDM_FIND:       ShowFindDlg(false); return 0;
        case IDM_REPLACE:    ShowFindDlg(true);  return 0;
        case IDM_FINDNEXT:
        case IDM_FINDPREV: {
            if (g_find.what.empty()) { ShowFindDlg(false); return 0; }
            bool saved = g_find.up;
            g_find.up = (id == IDM_FINDPREV) ? !saved : saved;
            FindNext(true);
            g_find.up = saved;
            return 0;
        }
        case IDM_GOTO:
            if (!g_wrap)
                DialogBoxParamW(GetModuleHandleW(NULL), MAKEINTRESOURCEW(IDD_GOTO),
                                h, GotoProc, 0);
            return 0;
        case IDM_BINGSEARCH: BingSearch();       return 0;
        case IDM_ZOOMIN:     SetZoom(g_zoom + 10); return 0;
        case IDM_ZOOMOUT:    SetZoom(g_zoom - 10); return 0;
        case IDM_ZOOMRESET:  SetZoom(100);       return 0;
        case IDM_STATUSBAR:  ToggleStatusBar();  return 0;
        case IDM_ABOUT:      About();            return 0;
        }
        break;
    }

    case WM_CLOSE:
        if (!ConfirmSave()) return 0;
        SaveSettings();
        DestroyWindow(h);
        return 0;

    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(h, msg, wp, lp);
}

// ---- 자체 검증 -------------------------------------------------------------
#ifdef SELFTEST
static void TestLayout() {
    g_root = NewPane();           // g_main == NULL → HWND 없이 트리만
    g_focus = g_root;
    RECT full = { 0, 0, 1000, 600 };

    // 단일 Pane은 영역 전체를 차지한다
    Layout(g_root, full);
    assert(g_root->rc.right == 1000 && g_root->rc.bottom == 600);

    // 좌우 분할: 새 Pane은 오른쪽, 간격 2px
    SplitFocus(DIR_V);
    Layout(g_root, full);
    assert(!g_root->isPane && g_root->dir == DIR_V);
    assert(g_root->a->rc.right == 499);
    assert(g_root->b->rc.left == 501);
    assert(g_root->b->rc.right == 1000);
    assert(g_root->sp.right - g_root->sp.left == GAP);
    assert(g_focus == g_root->b);                 // 새 Pane으로 포커스 이동

    // 상하 분할: 새 Pane은 아래쪽
    SplitFocus(DIR_H);
    Layout(g_root, full);
    Node* sp2 = g_root->b;
    assert(!sp2->isPane && sp2->dir == DIR_H);
    assert(sp2->a->rc.bottom == 299 && sp2->b->rc.top == 301);
    assert(LeafCount(g_root) == 3);

    // 순회 순서: 좌 → 우상 → 우하
    std::vector<Node*> v; CollectLeaves(g_root, v);
    assert(v.size() == 3 && v[0] == g_root->a && v[1] == sp2->a && v[2] == sp2->b);

    // 닫기: Split이 형제로 치환되고 남은 Pane이 공간을 채운다
    g_focus = sp2->b;
    CloseFocusPane();
    Layout(g_root, full);
    assert(LeafCount(g_root) == 2);
    assert(g_root->b->isPane && g_root->b->rc.bottom == 600);
    assert(g_focus == g_root->b);

    // ratio 조정이 반영된다
    g_root->ratio = 0.25f;
    Layout(g_root, full);
    assert(g_root->a->rc.right == 250 && g_root->b->rc.left == 252);

    // 히트 테스트: 분할선 위 / 밖
    POINT on  = { 250, 300 };
    POINT off = { 100, 300 };
    assert(HitSplitter(g_root, on) == g_root);
    assert(HitSplitter(g_root, off) == NULL);

    FreeSubtree(g_root); g_root = NULL; g_focus = NULL;
}

static void TestPath() {
    g_root = NewPane(); g_focus = g_root;
    SplitFocus(DIR_V);            // root = V(a=p1, b=p2)
    Node* p2 = g_root->b;
    SplitFocus(DIR_H);            // root = V(a=p1, b=H(a=p2, b=p3))
    Node* p3 = g_root->b->b;

    Path pp = PathOf(p3);
    assert(pp.size() == 2 && pp[0] == 1 && pp[1] == 1);
    assert(AtPath(pp) == p3);
    assert(AtPath(PathOf(p2)) == p2);
    assert(AtPath(Path()) == g_root);

    FreeSubtree(g_root); g_root = NULL; g_focus = NULL;
}

// 레이아웃 명령의 undo/redo가 트리를 원래대로 되돌린다 (SPEC §5)
static void TestLayoutUndo() {
    ResetUndo();
    g_root = NewPane(); g_focus = g_root;
    int id1 = g_root->id;

    SplitFocus(DIR_V);
    SplitFocus(DIR_H);
    assert(LeafCount(g_root) == 3 && g_pos == 2);

    Undo();
    assert(LeafCount(g_root) == 2 && g_pos == 1);
    Undo();
    assert(g_root->isPane && g_root->id == id1 && g_pos == 0);
    assert(!g_dirty);                      // 저장 시점으로 돌아오면 깨끗해진다

    Redo(); Redo();
    assert(LeafCount(g_root) == 3 && g_pos == 2);
    assert(!g_root->isPane && g_root->dir == DIR_V && g_root->b->dir == DIR_H);

    // 닫기 → undo로 공간과 내용이 되살아난다
    Node* closing = g_root->b->b;
    g_focus = closing;
    CloseFocusPane();
    assert(LeafCount(g_root) == 2 && g_pos == 3);
    Undo();
    assert(LeafCount(g_root) == 3 && g_pos == 2);
    assert(!g_root->b->isPane && g_root->b->dir == DIR_H);

    FreeSubtree(g_root); g_root = NULL; g_focus = NULL;
    ResetUndo();
}

// 타이핑 묶음 병합 규칙
static void TestMerge() {
    ResetUndo();
    g_root = NewPane(); g_focus = g_root;
    int id = g_root->id;

    Cmd c; c.kind = CMD_TEXT; c.pane = id; c.pos = 0; c.ins = L"a";
    assert(!TryMerge(c));
    PushCmd(c); g_runOpen = true;

    // 연속 입력은 하나로 합쳐진다
    Cmd d; d.kind = CMD_TEXT; d.pane = id; d.pos = 1; d.ins = L"b";
    assert(TryMerge(d));
    assert(g_cmds.size() == 1 && g_cmds[0].ins == L"ab");

    // 제자리 치환(IME 조합)도 합쳐진다
    Cmd e; e.kind = CMD_TEXT; e.pane = id; e.pos = 0; e.del = L"ab"; e.ins = L"ac";
    assert(TryMerge(e));
    assert(g_cmds.size() == 1 && g_cmds[0].ins == L"ac" && g_cmds[0].del.empty());

    // 줄바꿈을 넣을 때 끊어진다
    Cmd f; f.kind = CMD_TEXT; f.pane = id; f.pos = 2; f.ins = L"\r\n";
    assert(!TryMerge(f));

    // 줄바꿈 뒤에도 끊어진다 (묶음은 줄 단위로 끝난다)
    ResetUndo();
    Cmd n1; n1.kind = CMD_TEXT; n1.pane = id; n1.pos = 0; n1.ins = L"ab\r\n";
    PushCmd(n1); g_runOpen = true;
    Cmd n2; n2.kind = CMD_TEXT; n2.pane = id; n2.pos = 4; n2.ins = L"c";
    assert(!TryMerge(n2));

    // 다른 Pane이면 끊어진다
    Cmd g; g.kind = CMD_TEXT; g.pane = id + 99; g.pos = 2; g.ins = L"x";
    assert(!TryMerge(g));

    // 연속 백스페이스는 거꾸로 합쳐진다
    ResetUndo();
    Cmd h1; h1.kind = CMD_TEXT; h1.pane = id; h1.pos = 5; h1.del = L"e";
    PushCmd(h1); g_runOpen = true;
    Cmd h2; h2.kind = CMD_TEXT; h2.pane = id; h2.pos = 4; h2.del = L"d";
    assert(TryMerge(h2));
    assert(g_cmds[0].pos == 4 && g_cmds[0].del == L"de");

    FreeSubtree(g_root); g_root = NULL; g_focus = NULL;
    ResetUndo();
}

// .etxt 왕복: 트리 구조와 ratio가 보존된다
static void TestEtxtRoundTrip() {
    g_root = NewPane(); g_focus = g_root;
    SplitFocus(DIR_V);
    g_root->ratio = 0.25f;
    SplitFocus(DIR_H);
    g_focus = g_root->a;

    std::wstring w = BuildEtxt();
    assert(w.compare(0, 6, L"ETXT1\n") == 0);
    assert(w.find(L"\"focus\":0") != std::wstring::npos);
    assert(w.find(L"\"d\":\"V\"") != std::wstring::npos);
    assert(w.find(L"\"d\":\"H\"") != std::wstring::npos);
    assert(w.find(L'\0') != std::wstring::npos);        // Pane 구분자

    int focusIdx = -1; bool wrap = false;
    Node* r = ParseEtxt(w, focusIdx, wrap);
    assert(r && !r->isPane && r->dir == DIR_V);
    assert(r->ratio > 0.24f && r->ratio < 0.26f);
    assert(r->a->isPane && !r->b->isPane && r->b->dir == DIR_H);
    assert(LeafCount(r) == 3);
    assert(focusIdx == 0 && wrap == true);

    // 깨진 입력은 거부한다 (.txt로 처리되도록)
    int fi; bool wr;
    assert(ParseEtxt(L"그냥 평범한 텍스트", fi, wr) == NULL);
    assert(ParseEtxt(L"ETXT1\n{\"root\":{\"d\":\"V\",\"a\":{\"p\":0}}}\n", fi, wr) == NULL);

    // 본문 분리: 구분자 뒤부터 다음 \0 전까지가 그대로 본문이다
    std::wstring doc = L"ETXT1\nHEAD\n";
    doc += L'\0'; doc += L"PANE 0\n첫째\r\n줄";
    doc += L'\0'; doc += L"PANE 1\n";            // 빈 Pane
    doc += L'\0'; doc += L"PANE 2\n셋째";
    std::vector<std::wstring> bs = SplitEtxtBodies(doc, 6);
    assert(bs.size() == 3);
    assert(bs[0] == L"첫째\r\n줄" && bs[1] == L"" && bs[2] == L"셋째");

    FreeSubtree(r);
    FreeSubtree(g_root); g_root = NULL; g_focus = NULL;
    ResetUndo();
}

// 인코딩 왕복
static void TestEncoding() {
    const std::wstring s = L"한글 abc\r\n줄바꿈";
    for (int e = ENC_UTF8; e <= ENC_ANSI; ++e) {
        std::string bytes = EncodeText(s, e);
        int dec = e;
        assert(DecodeBytes(bytes, dec) == s);
    }
    // BOM이 설정보다 우선한다
    std::string u16 = EncodeText(s, ENC_UTF16LE);
    int dec = ENC_ANSI;
    assert(DecodeBytes(u16, dec) == s && dec == ENC_UTF16LE);
}

// 찾기 핵심: 방향, 대/소문자, 경계
static void TestFind() {
    const std::wstring t = L"abc ABC abc";
    assert(FindInText(t, L"abc", 0, false, true) == 0);
    assert(FindInText(t, L"abc", 1, false, true) == 8);     // 대/소문자 구분 → ABC 건너뜀
    assert(FindInText(t, L"abc", 1, false, false) == 4);    // 구분 안 함 → ABC에 걸린다
    assert(FindInText(t, L"abc", 9, false, true) == -1);    // 뒤에는 없다

    // 위로 찾기: from 앞쪽에서 가장 뒤의 것
    assert(FindInText(t, L"abc", 11, true, true) == 8);
    assert(FindInText(t, L"abc", 8, true, true) == 0);
    assert(FindInText(t, L"abc", 0, true, true) == -1);

    // 빈 문자열과 문서보다 긴 패턴
    assert(FindInText(t, L"", 0, false, true) == -1);
    assert(FindInText(t, L"abc abc abc abc", 0, false, true) == -1);

    // 한글도 그대로 (폴딩이 길이를 바꾸지 않는다)
    const std::wstring k = L"가나다 가나";
    assert(FindInText(k, L"가나", 0, false, false) == 0);
    assert(FindInText(k, L"가나", 1, false, false) == 4);

    // 모두 바꾸기
    std::wstring s = L"abc ABC abc";
    assert(ReplaceAllInText(s, L"abc", L"X", true) == 2 && s == L"X ABC X");
    s = L"abc ABC abc";
    assert(ReplaceAllInText(s, L"abc", L"X", false) == 3 && s == L"X X X");
    s = L"aaa";
    assert(ReplaceAllInText(s, L"a", L"aa", true) == 3 && s == L"aaaaaa");  // 무한 루프 없음
    s = L"abc";
    assert(ReplaceAllInText(s, L"zz", L"X", true) == 0 && s == L"abc");
    s = L"abc";
    assert(ReplaceAllInText(s, L"b", L"", true) == 1 && s == L"ac");
}

int main() {
    TestLayout();
    TestPath();
    TestLayoutUndo();
    TestMerge();
    TestEtxtRoundTrip();
    TestEncoding();
    TestFind();
    printf("selftest ok\n");
    return 0;
}
#else

int WINAPI wWinMain(HINSTANCE hi, HINSTANCE, LPWSTR, int) {
    SetProcessDPIAware();
    INITCOMMONCONTROLSEX ic = { sizeof(ic), ICC_STANDARD_CLASSES };
    InitCommonControlsEx(&ic);

    WNDCLASSEXW wc = {};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hi;
    wc.hCursor = LoadCursorW(NULL, IDC_ARROW);
    wc.hbrBackground = GetSysColorBrush(COLOR_3DSHADOW);   // Pane 사이 간격 색
    wc.lpszClassName = L"ExtendedNotepadMain";
    wc.hIcon = (HICON)LoadImageW(hi, MAKEINTRESOURCEW(IDI_APPICON), IMAGE_ICON,
                                 GetSystemMetrics(SM_CXICON),
                                 GetSystemMetrics(SM_CYICON), 0);
    wc.hIconSm = (HICON)LoadImageW(hi, MAKEINTRESOURCEW(IDI_APPICON), IMAGE_ICON,
                                   GetSystemMetrics(SM_CXSMICON),
                                   GetSystemMetrics(SM_CYSMICON), 0);
    RegisterClassExW(&wc);

    LoadSettings();

    // WS_CLIPCHILDREN: 부모가 Pane 위에 배경을 칠하지 않게 한다 (깜빡임의 주원인)
    g_main = CreateWindowExW(0, wc.lpszClassName, L"제목 없음 - 더 나은 메모장",
                             WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN,
                             CW_USEDEFAULT, CW_USEDEFAULT,
                             1000, 700, NULL, BuildMenuBar(), hi, NULL);
    if (!g_main) return 1;

    HMENU bar = GetMenu(g_main);
    CheckMenuRadioItem(bar, IDM_ENC_FIRST, IDM_ENC_LAST,
                       IDM_ENC_FIRST + g_enc, MF_BYCOMMAND);
    CheckMenuItem(bar, IDM_WORDWRAP, MF_BYCOMMAND | (g_wrap ? MF_CHECKED : MF_UNCHECKED));
    CheckMenuItem(bar, IDM_STATUSBAR, MF_BYCOMMAND | (g_statusOn ? MF_CHECKED : MF_UNCHECKED));

    g_status = CreateWindowExW(0, STATUSCLASSNAMEW, L"",
                               WS_CHILD | CCS_BOTTOM | SBARS_SIZEGRIP,
                               0, 0, 0, 0, g_main, NULL, hi, NULL);
    if (g_status && g_statusOn) ShowWindow(g_status, SW_SHOW);

    g_root = NewPane();
    g_focus = g_root;
    ApplyFont();                 // g_font 생성 + 모든 Pane 적용 + 배치
    LayoutStatus();

    // 지난번에 쓰던 자리로 되돌린다 (보이기 전에 해야 창이 튀지 않는다)
    int show = SW_SHOW;
    if (g_hasPlace && PlaceOnScreen(g_place)) {
        WINDOWPLACEMENT wp;
        ZeroMemory(&wp, sizeof(wp));
        wp.length = sizeof(wp);
        wp.rcNormalPosition = g_place;
        wp.showCmd = g_placeMax ? SW_SHOWMAXIMIZED : SW_SHOWNORMAL;
        SetWindowPlacement(g_main, &wp);
        show = g_placeMax ? SW_SHOWMAXIMIZED : SW_SHOW;
    }
    ShowWindow(g_main, show);
    SetFocus(g_root->edit);

    // .etxt 파일 연결로 실행될 때는 인수로 경로가 온다
    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (argv) {
        if (argc > 1) LoadFile(argv[1]);
        LocalFree(argv);
    }

    ACCEL acc[] = {
        { FVIRTKEY | FCONTROL, 'N', IDM_NEW },
        { FVIRTKEY | FCONTROL | FSHIFT, 'N', IDM_NEWWIN },
        { FVIRTKEY | FCONTROL, 'O', IDM_OPEN },
        { FVIRTKEY | FCONTROL, 'S', IDM_SAVE },
        { FVIRTKEY | FCONTROL | FSHIFT, 'S', IDM_SAVEAS },
        { FVIRTKEY | FCONTROL | FALT, 'S', IDM_SAVE_PANE },
        { FVIRTKEY | FCONTROL, 'Z', IDM_UNDO },
        { FVIRTKEY | FCONTROL | FSHIFT, 'Z', IDM_REDO },
        { FVIRTKEY | FCONTROL, 'Y', IDM_REDO },
        // 여러 줄 EDIT은 Ctrl+A를 처리하지 않는다. 메모장처럼 직접 건다.
        { FVIRTKEY | FCONTROL, 'A', IDM_SELECT_ALL },
        { FVIRTKEY | FCONTROL, 'R', IDM_SPLIT_V },
        { FVIRTKEY | FCONTROL, 'T', IDM_SPLIT_H },
        { FVIRTKEY | FCONTROL, 'W', IDM_CLOSE_PANE },
        { FVIRTKEY | FCONTROL, VK_TAB, IDM_NEXT_PANE },
        { FVIRTKEY | FCONTROL | FSHIFT, VK_TAB, IDM_PREV_PANE },
        { FVIRTKEY, VK_F5, IDM_TIMEDATE },
        { FVIRTKEY | FCONTROL, 'F', IDM_FIND },
        { FVIRTKEY, VK_F3, IDM_FINDNEXT },
        { FVIRTKEY | FSHIFT, VK_F3, IDM_FINDPREV },
        { FVIRTKEY | FCONTROL, 'H', IDM_REPLACE },
        { FVIRTKEY | FCONTROL, 'G', IDM_GOTO },
        { FVIRTKEY | FCONTROL, 'E', IDM_BINGSEARCH },
        { FVIRTKEY | FCONTROL, 'P', IDM_PRINT },
        { FVIRTKEY | FCONTROL, VK_OEM_PLUS, IDM_ZOOMIN },
        { FVIRTKEY | FCONTROL, VK_ADD, IDM_ZOOMIN },
        { FVIRTKEY | FCONTROL, VK_OEM_MINUS, IDM_ZOOMOUT },
        { FVIRTKEY | FCONTROL, VK_SUBTRACT, IDM_ZOOMOUT },
        { FVIRTKEY | FCONTROL, '0', IDM_ZOOMRESET },
    };
    HACCEL ha = CreateAcceleratorTableW(acc, (int)(sizeof(acc) / sizeof(acc[0])));

    MSG m;
    while (GetMessageW(&m, NULL, 0, 0) > 0) {
        // 찾기/바꾸기는 모달이 아니다 — 먼저 그 창에 넘긴다
        if (g_findDlg && IsDialogMessageW(g_findDlg, &m)) continue;
        if (!TranslateAcceleratorW(g_main, ha, &m)) {
            TranslateMessage(&m);
            DispatchMessageW(&m);
        }
    }
    return (int)m.wParam;
}
#endif
