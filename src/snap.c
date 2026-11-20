/* The elements under the mouse while an area is chosen, when the setting is
 * DETECT_ELEMENTS: the window and its child windows at once, and what UI Automation
 * knows inside them once a thread has asked. Asking takes as long as the program asked
 * likes, so neither a mouse move, the capture nor its end ever waits for it.
 *
 * A session lasts as long as the choosing. Its thread follows the mouse: one search at
 * a time, for the newest place, below the window there. A search keeps the neighbours
 * of what it passes too, so an element is usually known by the time the mouse gets to
 * it. When the session ends the thread finishes the call it is in, lets go of UI
 * Automation and ends; the session's memory goes with whichever of the two is last. */
#include "snap.h"
#include "shot.h"
#include "ov_layout.inc"
#include <uiautomationclient.h>
#include <dwmapi.h>

#define SNAP_INTERVAL 24 /* ms from one search to the next: the mouse is followed, not waited for */
#define SNAP_BUDGET 150  /* ms after which a search asks nothing more */
#define SNAP_TIMEOUT 200 /* ms a program has for an answer */
#define SNAP_DEPTH 32
#define SNAP_NODES 512   /* elements a search looks at */
#define SNAP_CACHE 512   /* elements kept of the window under the mouse */
#define SNAP_INPUTS 48   /* of those, and of the child windows: the ones under the mouse */

extern uint8_t O[]; /* the capture's state, laid out as ov_layout.inc says */
#define STATE(type, offset) (*(type *)(O + (offset)))

typedef struct {
    HWND h;
    RECT rc;
} Win; /* an entry of O.win */
_Static_assert(sizeof(Win) == WIN_BYTES && sizeof(RECT) == OV_CHAIN_BYTES, "the capture's layout");

typedef struct {
    HWND target;
    RECT bounds;
    POINT point;
    DWORD pid;
    uint64_t version; /* counts the targets: a search for an earlier one is dropped */
} Request;

typedef struct {
    LONG refs; /* the capture's and the thread's */
    HWND overlay;
    HANDLE wake;
    SRWLOCK lock; /* for what both threads use: */
    bool active, pending, ready;
    Request request;
    uint64_t version;
    int nresult;
    RECT result[SNAP_CACHE];
    /* the capture's thread only */
    RECT leaf, selected;
    RECT cached[SNAP_CACHE], incoming[SNAP_CACHE];
    int ncached, next_cache;
    bool manual, have_leaf;
    /* the session's thread only */
    RECT found[SNAP_CACHE];
} Session;

Session *snap_session; /* while an area is chosen with elements; ov_show.S asks whether there is one */

static bool contains(const RECT *outer, const RECT *inner)
{
    return outer->left <= inner->left && outer->top <= inner->top && outer->right >= inner->right &&
           outer->bottom >= inner->bottom;
}

static uint64_t area(const RECT *r)
{
    return (uint64_t)((int64_t)r->right - r->left) * (uint64_t)((int64_t)r->bottom - r->top);
}

static RECT moved(RECT r, int dx, int dy)
{
    return (RECT){r.left + dx, r.top + dy, r.right + dx, r.bottom + dy};
}

/* r, cut to clip, joins the candidates when it is under the point and not among them.
 * When they are full the largest gives way: a window with many elements under one
 * point keeps the finest. */
static void add_rect(RECT *rects, int *n, const RECT *r, const RECT *clip, POINT point)
{
    if (!rect_has(r, point.x, point.y))
        return;
    RECT cut = rect_isect(*r, *clip);
    if (!rect_has(&cut, point.x, point.y))
        return;
    for (int i = 0; i < *n; i++)
        if (rect_eq(&rects[i], &cut))
            return;
    if (*n < SNAP_INPUTS) {
        rects[(*n)++] = cut;
        return;
    }
    int largest = 0;
    for (int i = 1; i < *n; i++)
        if (area(&rects[i]) > area(&rects[largest]))
            largest = i;
    if (area(&cut) < area(&rects[largest]))
        rects[largest] = cut;
}

/* The chain from the candidates: smallest first, each inside the next. The window's
 * client area, the window and the monitor keep their places at the end however many
 * there are. The monitor comes last even when it does not hold the window: a window
 * can lie across two. */
static int merge_rects(RECT *out, RECT *rects, int n, RECT client, RECT window, RECT monitor, POINT point)
{
    for (int i = 1; i < n; i++) {
        RECT r = rects[i];
        int j = i;
        for (; j && area(&rects[j - 1]) > area(&r); j--)
            rects[j] = rects[j - 1];
        rects[j] = r;
    }
    int used = 0;
    for (int i = 0; i < n && used < MAX_CHAIN - 3; i++)
        if (!rect_eq(&rects[i], &client) && !rect_eq(&rects[i], &window) &&
            (!used || contains(&rects[i], &out[used - 1])))
            out[used++] = rects[i];
    const RECT coarse[3] = {client, window, monitor};
    for (int i = 0; i < 3; i++) {
        const RECT *r = &coarse[i];
        /* between two displays the monitor is the nearest one: the point is not in it */
        if (rect_empty(r) || (i < 2 && !rect_has(r, point.x, point.y)))
            continue;
        bool repeat = false;
        for (int j = 0; j < used; j++)
            repeat |= rect_eq(&out[j], r);
        if (!repeat && (i == 2 || !used || contains(r, &out[used - 1])))
            out[used++] = *r;
    }
    return used;
}

/* as collect_windows (ov_base.S) measures a window */
static bool window_bounds(HWND target, RECT *bounds)
{
    return SUCCEEDED(DwmGetWindowAttribute(target, DWMWA_EXTENDED_FRAME_BOUNDS, bounds, sizeof *bounds)) ||
           GetWindowRect(target, bounds);
}

static bool current_request(Session *s, const Request *r)
{
    AcquireSRWLockExclusive(&s->lock);
    bool current = s->active && s->version == r->version;
    ReleaseSRWLockExclusive(&s->lock);
    return current;
}

/* The window is still there, the same one, where the capture saw it. */
static bool valid_target(const Request *r)
{
    DWORD pid = 0;
    RECT bounds;
    return IsWindow(r->target) && IsWindowVisible(r->target) && !IsIconic(r->target) &&
           GetWindowThreadProcessId(r->target, &pid) && pid == r->pid && window_bounds(r->target, &bounds) &&
           rect_eq(&bounds, &r->bounds);
}

/* ---- the session's thread ---- */

typedef struct {
    IUIAutomation2 *automation;
    IUIAutomationCacheRequest *cache;
    IUIAutomationCondition *all;
    void(WINAPI *leave)(void); /* CoUninitialize, once the thread is in its apartment */
} Provider;

typedef struct {
    Session *session;
    const Request *request;
    Provider *provider;
    DWORD start;    /* GetTickCount */
    int visited, n; /* elements looked at; rectangles in session->found */
} Search;

static void search_cache(Search *q, RECT bounds)
{
    RECT *found = q->session->found, r = rect_isect(bounds, q->request->bounds);
    if (rect_empty(&r))
        return;
    for (int i = 0; i < q->n; i++)
        if (rect_eq(&found[i], &r))
            return;
    if (q->n < SNAP_CACHE)
        found[q->n++] = r;
}

static bool search_more(Search *q)
{
    return q->visited < SNAP_NODES && GetTickCount() - q->start < SNAP_BUDGET && current_request(q->session, q->request);
}

static bool component_type(CONTROLTYPEID type)
{
    return type != UIA_TextControlTypeId && type != UIA_ToolTipControlTypeId && type != UIA_SeparatorControlTypeId;
}

/* The raw tree below parent, one call per level, which brings everything read here.
 * Kept: the controls that show, the neighbours of the ones under the point too. Only
 * what is under the point is entered, and what has no rectangle: that is structure,
 * not something hidden. */
static void search_children(Search *q, IUIAutomationElement *parent, int depth)
{
    const Provider *p = q->provider;
    const POINT at = q->request->point;
    IUIAutomationElementArray *children = NULL;
    int length = 0;
    if (depth >= SNAP_DEPTH || !search_more(q) ||
        FAILED(IUIAutomationElement_FindAllBuildCache(parent, TreeScope_Children, p->all, p->cache, &children)) ||
        !children)
        return;
    IUIAutomationElementArray_get_Length(children, &length);
    /* the level is here, even if it took longer than the budget: all of it is used */
    for (int i = 0; i < length && q->visited < SNAP_NODES && current_request(q->session, q->request); i++) {
        IUIAutomationElement *e = NULL;
        RECT bounds = {0};
        BOOL offscreen = FALSE, control = FALSE;
        CONTROLTYPEID type = 0;
        q->visited++;
        if (FAILED(IUIAutomationElementArray_GetElement(children, i, &e)) || !e)
            continue;
        if (SUCCEEDED(IUIAutomationElement_get_CachedBoundingRectangle(e, &bounds)) &&
            SUCCEEDED(IUIAutomationElement_get_CachedIsOffscreen(e, &offscreen)) && !offscreen) {
            bool empty = rect_empty(&bounds);
            if (!empty && SUCCEEDED(IUIAutomationElement_get_CachedIsControlElement(e, &control)) && control &&
                SUCCEEDED(IUIAutomationElement_get_CachedControlType(e, &type)) && component_type(type))
                search_cache(q, bounds);
            if (empty || rect_has(&bounds, at.x, at.y))
                search_children(q, e, depth + 1);
        }
        IUIAutomationElement_Release(e);
    }
    IUIAutomationElementArray_Release(children);
}

/* UI Automation for this thread, in an apartment of its own kind (multithreaded: it
 * has no message loop). ole32 is loaded once, when first needed, and stays, like image.S's. */
static bool provider_open(Provider *p)
{
    typedef HRESULT(WINAPI *PFN_CoInitializeEx)(void *, DWORD);
    typedef HRESULT(WINAPI *PFN_CoCreateInstance)(REFCLSID, IUnknown *, DWORD, REFIID, void **);
    typedef void(WINAPI *PFN_CoUninitialize)(void);
    static const PROPERTYID kRead[] = {UIA_BoundingRectanglePropertyId, UIA_ControlTypePropertyId,
                                       UIA_IsControlElementPropertyId, UIA_IsOffscreenPropertyId};
    HMODULE ole = GetModuleHandleW(L"ole32.dll");
    if (!ole)
        ole = LoadLibraryW(L"ole32.dll");
    PFN_CoInitializeEx init = ole ? (PFN_CoInitializeEx)(void *)GetProcAddress(ole, "CoInitializeEx") : NULL;
    PFN_CoCreateInstance create = ole ? (PFN_CoCreateInstance)(void *)GetProcAddress(ole, "CoCreateInstance") : NULL;
    PFN_CoUninitialize leave = ole ? (PFN_CoUninitialize)(void *)GetProcAddress(ole, "CoUninitialize") : NULL;
    if (!init || !create || !leave || FAILED(init(NULL, COINIT_MULTITHREADED)))
        return false;
    p->leave = leave;
    if (FAILED(create(&CLSID_CUIAutomation8, NULL, CLSCTX_INPROC_SERVER, &IID_IUIAutomation2, (void **)&p->automation)) ||
        FAILED(IUIAutomation2_put_ConnectionTimeout(p->automation, SNAP_TIMEOUT)) ||
        FAILED(IUIAutomation2_put_TransactionTimeout(p->automation, SNAP_TIMEOUT)) ||
        FAILED(IUIAutomation2_CreateCacheRequest(p->automation, &p->cache)) ||
        FAILED(IUIAutomation2_CreateTrueCondition(p->automation, &p->all)))
        return false;
    for (int i = 0; i < COUNTOF(kRead); i++)
        if (FAILED(IUIAutomationCacheRequest_AddProperty(p->cache, kRead[i])))
            return false;
    return SUCCEEDED(IUIAutomationCacheRequest_put_TreeScope(p->cache, TreeScope_Element)) &&
           SUCCEEDED(IUIAutomationCacheRequest_put_TreeFilter(p->cache, p->all)) &&
           SUCCEEDED(IUIAutomationCacheRequest_put_AutomationElementMode(p->cache, AutomationElementMode_Full));
}

/* What this thread made is given back; uiautomationcore.dll stays, with threads of its own. */
static void provider_close(Provider *p)
{
    if (p->all)
        IUIAutomationCondition_Release(p->all);
    if (p->cache)
        IUIAutomationCacheRequest_Release(p->cache);
    if (p->automation)
        IUIAutomation2_Release(p->automation);
    if (p->leave)
        p->leave();
}

/* The newest request, SNAP_INTERVAL after the search before began at the earliest.
 * false: the session is over. */
static bool next_request(Session *s, DWORD last, Request *r)
{
    for (;;) {
        DWORD since = GetTickCount() - last, wait = INFINITE;
        AcquireSRWLockExclusive(&s->lock);
        bool active = s->active, take = active && s->pending && since >= SNAP_INTERVAL;
        if (take) {
            *r = s->request;
            s->pending = false;
        } else if (s->pending) {
            wait = SNAP_INTERVAL - since;
        }
        ReleaseSRWLockExclusive(&s->lock);
        if (!active || take)
            return active;
        WaitForSingleObject(s->wake, wait);
    }
}

/* What a search found is the session's result if its window is still the one asked
 * about, and as it was. The capture is told. */
static void publish(Session *s, const Request *r, int n)
{
    bool valid = valid_target(r);
    AcquireSRWLockExclusive(&s->lock);
    bool current = valid && s->active && s->version == r->version;
    if (current) {
        memcpy(s->result, s->found, (size_t)n * sizeof(RECT));
        s->nresult = n;
        s->ready = true;
    }
    ReleaseSRWLockExclusive(&s->lock);
    if (current)
        PostMessageW(s->overlay, WM_APP_SNAP_READY, 0, 0);
}

static void session_release(Session *s)
{
    if (InterlockedDecrement(&s->refs))
        return;
    CloseHandle(s->wake);
    mem_free(s);
}

static DWORD WINAPI snap_worker(void *arg)
{
    Session *s = arg;
    Provider p = {0};
    Request r;
    DWORD start = GetTickCount() - SNAP_INTERVAL; /* the first search need not wait */
    if (provider_open(&p)) /* else: windows and child windows are all there is */
        while (next_request(s, start, &r)) {
            start = GetTickCount();
            Search q = {s, &r, &p, start, 0, 0};
            IUIAutomationElement *root = NULL;
            if (valid_target(&r) && SUCCEEDED(IUIAutomation2_ElementFromHandle(p.automation, r.target, &root)) && root) {
                search_children(&q, root, 0);
                IUIAutomationElement_Release(root);
            }
            publish(s, &r, q.n);
        }
    provider_close(&p);
    session_release(s);
    return 0;
}

/* ---- the capture's thread ---- */

/* The thread is to look below target at the point; a request not yet begun is replaced.
 * true: another window than before. */
static bool snap_request(Session *s, HWND target, const RECT *bounds, POINT point)
{
    DWORD pid = 0;
    if (target && GetWindowThreadProcessId(target, &pid) && pid == GetCurrentProcessId())
        target = NULL; /* a window of the tool's own: UI Automation would keep more threads and handles for good */
    AcquireSRWLockExclusive(&s->lock);
    Request *r = &s->request;
    bool new_target = r->target != target || r->pid != pid || !rect_eq(&r->bounds, bounds);
    bool changed = new_target || r->point.x != point.x || r->point.y != point.y, wake = false;
    if (changed) {
        if (new_target) {
            s->version++;
            s->ready = false;
        }
        *r = (Request){target, *bounds, point, pid, s->version};
        wake = target && !s->pending; /* a request already pending is taken when the thread's wait ends */
        s->pending = target != NULL;
    }
    ReleaseSRWLockExclusive(&s->lock);
    if (wake)
        SetEvent(s->wake);
    return new_target;
}

/* The window of the capture's list at the point, the topmost one; NULL: none. */
static HWND window_at(POINT point, RECT *bounds)
{
    const Win *w = STATE(const Win *, OV_WIN);
    for (int i = 0; i < STATE(int, OV_NWIN); i++)
        if (rect_has(&w[i].rc, point.x, point.y)) {
            *bounds = w[i].rc;
            return w[i].h;
        }
    return NULL;
}

void snap_end(void)
{
    Session *s = snap_session;
    snap_session = NULL;
    AcquireSRWLockExclusive(&s->lock);
    s->active = false;
    ReleaseSRWLockExclusive(&s->lock);
    SetEvent(s->wake);
    session_release(s);
}

void snap_begin(void)
{
    Session *s = mem_alloc(sizeof *s);
    if (!s)
        return;
    s->refs = 2;
    s->active = true;
    s->overlay = STATE(HWND, OV_HWND);
    s->wake = CreateEventW(NULL, FALSE, FALSE, NULL);
    HANDLE thread = s->wake ? CreateThread(NULL, 0, snap_worker, s, 0, NULL) : NULL;
    if (!thread) { /* snap_update does without */
        if (s->wake)
            CloseHandle(s->wake);
        mem_free(s);
        return;
    }
    CloseHandle(thread);
    const Shot *shot = (const Shot *)(O + OV_SHOT);
    const POINT cur = STATE(POINT, OV_CUR), point = {cur.x + shot->vx, cur.y + shot->vy};
    RECT bounds = {0};
    snap_request(s, window_at(point, &bounds), &bounds, point); /* searched at once, not only after a move */
    s->leaf = moved(STATE(RECT, OV_CHAIN), shot->vx, shot->vy); /* update_chain's innermost: the wheel's choice holds on it */
    s->have_leaf = true;
    snap_session = s;
}

/* A result not yet seen, into s->incoming: how many, or -1 when there is none. */
static int snap_take(Session *s)
{
    int n = -1;
    AcquireSRWLockExclusive(&s->lock);
    if (s->ready) {
        n = s->nresult;
        memcpy(s->incoming, s->result, (size_t)n * sizeof(RECT));
        s->ready = false;
    }
    ReleaseSRWLockExclusive(&s->lock);
    return n;
}

/* The cache takes what it does not have yet; when it is full the oldest go. */
static void cache_merge(Session *s, int n)
{
    for (int i = 0; i < n; i++) {
        const RECT *r = &s->incoming[i];
        int j = 0;
        while (j < s->ncached && !rect_eq(r, &s->cached[j]))
            j++;
        if (j < s->ncached)
            continue;
        if (s->ncached < SNAP_CACHE) {
            s->cached[s->ncached++] = *r;
        } else {
            s->cached[s->next_cache] = *r;
            s->next_cache = (s->next_cache + 1) % SNAP_CACHE;
        }
    }
}

void snap_remember(void)
{
    Session *s = snap_session;
    const Shot *shot = (const Shot *)(O + OV_SHOT);
    if (!s)
        return;
    s->selected = moved(STATE(RECT, OV_HL), shot->vx, shot->vy);
    s->manual = true;
}

/* The session follows the mouse to this window and point: its cache is of one window,
 * what the wheel chose holds while the mouse is on the same innermost rectangle. The
 * thread is asked, and what it has found since the last time joins the cache.
 * Returns how many rectangles that was, or -1 when it had nothing. */
static int session_follow(Session *s, HWND target, const RECT *bounds, POINT point)
{
    bool new_target = snap_request(s, target, bounds, point);
    if (new_target)
        s->ncached = s->next_cache = 0;
    if (new_target || (s->have_leaf && !rect_has(&s->leaf, point.x, point.y)))
        s->manual = s->have_leaf = false;
    int received = snap_take(s);
    if (received > 0)
        cache_merge(s, received);
    return received;
}

/* The level shown: the innermost, or the one the wheel chose. A choice outlives the
 * arrival of finer rectangles (received >= 0), not another change of the innermost. */
static int session_level(Session *s, const RECT *chain, int count, int received)
{
    int level = 0;
    if (received < 0 && !(s->have_leaf && rect_eq(&chain[0], &s->leaf)))
        s->manual = false;
    if (s->manual)
        for (int i = 0; i < count; i++)
            if (rect_eq(&chain[i], &s->selected)) {
                level = i;
                break;
            }
    s->leaf = chain[0];
    s->have_leaf = true;
    return level;
}

/* O.chain, O.nchain, O.level and O.hl for where the mouse is, as update_chain (ov_base.S)
 * leaves them. Without a session: from the window and its child windows alone. */
void snap_update(void)
{
    if (STATE(int, OV_PHASE) != PH_SELECT || STATE(bool, OV_LDOWN))
        return;
    Session *s = snap_session;
    const Shot *shot = (const Shot *)(O + OV_SHOT);
    const POINT cur = STATE(POINT, OV_CUR), point = {cur.x + shot->vx, cur.y + shot->vy};
    RECT desktop = {shot->vx, shot->vy, shot->vx + shot->vw, shot->vy + shot->vh};
    RECT monitor = moved(shot->tile[shot_tile_at(shot, cur.x, cur.y)].rc, shot->vx, shot->vy);
    RECT bounds = {0}, client = {0}, r;
    HWND target = window_at(point, &bounds);
    RECT window = rect_isect(bounds, desktop), clip = target ? window : desktop;

    RECT rects[SNAP_INPUTS]; /* what is under the mouse: of the session's elements, then the child windows */
    int n = 0, received = -1;
    if (s) {
        received = session_follow(s, target, &bounds, point);
        for (int i = 0; i < s->ncached; i++)
            add_rect(rects, &n, &s->cached[i], &clip, point);
    }
    HWND child = target;
    for (int depth = 0; child && depth < 12; depth++) { /* as update_chain walks them */
        POINT local = point;
        if (!ScreenToClient(child, &local))
            break;
        HWND next = ChildWindowFromPointEx(child, local, CWP_SKIPINVISIBLE | CWP_SKIPTRANSPARENT);
        if (!next || next == child || !GetWindowRect(next, &r))
            break;
        add_rect(rects, &n, &r, &clip, point);
        child = next;
    }
    if (target && GetClientRect(target, &client))
        MapWindowPoints(target, NULL, (POINT *)&client, 2); /* right for a mirrored window too */

    RECT *chain = (RECT *)(O + OV_CHAIN);
    int count = merge_rects(chain, rects, n, rect_isect(client, clip), window, monitor, point);
    int level = s ? session_level(s, chain, count, received) : CLAMPI(STATE(int, OV_LEVEL), 0, count - 1);
    for (int i = 0; i < count; i++)
        chain[i] = moved(chain[i], -shot->vx, -shot->vy);
    STATE(int, OV_NCHAIN) = count;
    STATE(int, OV_LEVEL) = level;
    STATE(RECT, OV_HL) = chain[level];
}
