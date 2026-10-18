// Drag-to-arrange finite automaton viewer.
//
// Usage: dfa [--screenshot out.png] automaton.dot
//
// The DOT file is laid out by Graphviz (`dot -Tplain`) for the starting
// positions. Conventions:
//   shape=doublecircle                    accepting state
//   edge from a shape=point, style=invis  marks its target as initial
//     or unlabelled shape=none node
//   several edges between the same pair   merged, labels joined with ", "
//
// Controls:
//   left-drag   move a state
//   G           toggle grid snapping
//   P           print state positions (in grid units) to stdout

#define _POSIX_C_SOURCE 200809L  // popen, getline, strdup

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "raylib.h"
#include "raymath.h"

#define NODE_R      28.0f
#define ACCEPT_GAP  5.0f
#define GRID        60.0f
#define BEND_DEG    25.0f
#define ARROW_LEN   12.0f
#define ARROW_W     5.0f
#define STROKE      2.0f
#define FONT_SIZE   20
#define ARC_SEGS    48
#define INIT_LEN    40.0f

#define MAX_STATES  128
#define MAX_EDGES   512
#define PX_PER_INCH 150.0f   // Graphviz packs nodes tightly; spread them out a bit
#define MARGIN      70.0f
#define MIN_W       880
#define MIN_H       560
#define MAX_W       1600
#define MAX_H       1000

typedef struct {
    char *name;      // DOT node id
    char *label;
    Vector2 pos;
    bool accepting;
    bool initial;
    Vector2 init_dir;  // direction the initial arrow comes from
} State;

typedef struct {
    int from, to;
    char *label;
} Edge;

static State states[MAX_STATES];
static int n_states;
static Edge edges[MAX_EDGES];
static int n_edges;

static const Color BG     = { 250, 249, 246, 255 };
static const Color INK    = {  30,  30,  35, 255 };
static const Color GRIDC  = { 225, 222, 215, 255 };
static const Color HOVER  = { 230, 238, 250, 255 };
static const Color ACTIVE = { 205, 222, 248, 255 };

// ---------------------------------------------------------------------------
// DOT import via `dot -Tplain`

// Split a -Tplain line into tokens in place. Quoted strings are unquoted and
// their \" escapes resolved.
static int Tokenize(char *s, char **tok, int max) {
    int n = 0;
    while (n < max) {
        while (*s == ' ' || *s == '\n' || *s == '\r') s++;
        if (!*s) break;
        if (*s == '"') {
            char *out = ++s;
            tok[n++] = out;
            while (*s && *s != '"') {
                if (s[0] == '\\' && s[1] == '"') s++;
                *out++ = *s++;
            }
            if (*s) s++;
            *out = '\0';
        } else {
            tok[n++] = s;
            while (*s && *s != ' ' && *s != '\n' && *s != '\r') s++;
            if (*s) *s++ = '\0';
        }
    }
    return n;
}

// Wrap a string in single quotes for /bin/sh.
static char *ShellQuote(const char *s) {
    char *out = malloc(strlen(s) * 4 + 3), *o = out;
    *o++ = '\'';
    for (; *s; s++) {
        if (*s == '\'') { memcpy(o, "'\\''", 4); o += 4; }
        else *o++ = *s;
    }
    *o++ = '\'';
    *o = '\0';
    return out;
}

typedef struct {
    char *name, *label;
    Vector2 pos;   // inches, y up
    bool marker;   // invisible start marker, not a state
    bool accepting;
    int start;     // marker node pointing at this one, or -1
    int state;     // index into states[], or -1 for markers
} DotNode;

typedef struct {
    int tail, head;  // indices into the DotNode array
    char *label;
} DotEdge;

static int FindNode(DotNode *nodes, int n, const char *name) {
    for (int i = 0; i < n; i++)
        if (strcmp(nodes[i].name, name) == 0) return i;
    return -1;
}

static void AddEdge(int from, int to, const char *label) {
    for (int i = 0; i < n_edges; i++) {
        if (edges[i].from != from || edges[i].to != to) continue;
        if (*label) {
            char *joined = malloc(strlen(edges[i].label) + strlen(label) + 3);
            sprintf(joined, *edges[i].label ? "%s, %s" : "%s%s", edges[i].label, label);
            free(edges[i].label);
            edges[i].label = joined;
        }
        return;
    }
    if (n_edges == MAX_EDGES) return;
    edges[n_edges++] = (Edge){ from, to, strdup(label) };
}

// Load the automaton and lay it out to fit a window of size *w x *h.
static bool LoadDot(const char *path, int *w, int *h) {
    char *quoted = ShellQuote(path);
    char *cmd = malloc(strlen(quoted) + 32);
    sprintf(cmd, "dot -Tplain %s", quoted);
    FILE *f = popen(cmd, "r");
    free(cmd);
    free(quoted);
    if (!f) { perror("popen"); return false; }

    static DotNode nodes[MAX_STATES * 2];
    static DotEdge dedges[MAX_EDGES * 2];
    int n_nodes = 0, n_dedges = 0;
    float gw = 0, gh = 0;
    char *line = NULL;
    size_t cap = 0;
    ssize_t len;

    while ((len = getline(&line, &cap, f)) > 0) {
        char **tok = malloc(sizeof(char *) * (len / 2 + 2));
        int n = Tokenize(line, tok, len / 2 + 2);

        if (n >= 4 && strcmp(tok[0], "graph") == 0) {
            gw = atof(tok[2]);
            gh = atof(tok[3]);
        } else if (n >= 11 && strcmp(tok[0], "node") == 0 && n_nodes < MAX_STATES * 2) {
            // node name x y width height label style shape color fillcolor
            const char *label = tok[6], *style = tok[7], *shape = tok[8];
            bool marker = strcmp(shape, "point") == 0 || strstr(style, "invis") ||
                          ((strcmp(shape, "none") == 0 || strcmp(shape, "plaintext") == 0) && !*label);
            nodes[n_nodes++] = (DotNode){
                .name = strdup(tok[1]), .label = strdup(label),
                .pos = { atof(tok[2]), atof(tok[3]) },
                .marker = marker, .accepting = strcmp(shape, "doublecircle") == 0,
                .start = -1, .state = -1,
            };
        } else if (n >= 4 && strcmp(tok[0], "edge") == 0 && n_dedges < MAX_EDGES * 2) {
            // edge tail head n x1 y1 .. xn yn [label xl yl] style color
            int a = FindNode(nodes, n_nodes, tok[1]), b = FindNode(nodes, n_nodes, tok[2]);
            int after = 4 + 2 * atoi(tok[3]);
            if (a >= 0 && b >= 0)
                dedges[n_dedges++] = (DotEdge){ a, b, strdup(n - after == 5 ? tok[after] : "") };
        }
        free(tok);
    }
    free(line);
    if (pclose(f) != 0) {
        fprintf(stderr, "dfa: `dot -Tplain` failed on %s\n", path);
        return false;
    }

    for (int i = 0; i < n_dedges; i++)
        if (nodes[dedges[i].tail].marker && !nodes[dedges[i].head].marker)
            nodes[dedges[i].head].start = dedges[i].tail;

    // Graphviz inches (y up) -> pixels (y down), scaled down if too big for MAX_W x MAX_H.
    float scale = PX_PER_INCH;
    scale = fminf(scale, (MAX_W - 2 * MARGIN) / fmaxf(gw, 1e-3f));
    scale = fminf(scale, (MAX_H - 2 * MARGIN) / fmaxf(gh, 1e-3f));
    *w = (int)fmaxf(MIN_W, gw * scale + 2 * MARGIN);
    *h = (int)fmaxf(MIN_H, gh * scale + 2 * MARGIN);
    Vector2 offset = { (*w - gw * scale) / 2, (*h - gh * scale) / 2 };

    for (int i = 0; i < n_nodes && n_states < MAX_STATES; i++) {
        DotNode *dn = &nodes[i];
        if (dn->marker) continue;
        dn->state = n_states;
        State *s = &states[n_states++];
        *s = (State){
            .name = dn->name, .label = dn->label, .accepting = dn->accepting,
            .pos = { offset.x + dn->pos.x * scale, offset.y + (gh - dn->pos.y) * scale },
        };
        if (dn->start >= 0) {
            Vector2 d = Vector2Subtract(nodes[dn->start].pos, dn->pos);
            s->initial = true;
            s->init_dir = Vector2Normalize((Vector2){ d.x, -d.y });
        }
    }
    if (n_states == 0) {
        fprintf(stderr, "dfa: no states found in %s\n", path);
        return false;
    }

    for (int i = 0; i < n_dedges; i++) {
        int a = nodes[dedges[i].tail].state, b = nodes[dedges[i].head].state;
        if (a >= 0 && b >= 0) AddEdge(a, b, dedges[i].label);
        free(dedges[i].label);
    }
    return true;
}

// ---------------------------------------------------------------------------
// Drawing

static float OuterRadius(int s) {
    return states[s].accepting ? NODE_R + ACCEPT_GAP : NODE_R;
}

static Vector2 Perp(Vector2 v) { return (Vector2){ -v.y, v.x }; }

// raylib wants counter-clockwise vertices; accept either winding.
static void FillTriangle(Vector2 a, Vector2 b, Vector2 c, Color col) {
    float cross = (b.x - a.x) * (c.y - a.y) - (b.y - a.y) * (c.x - a.x);
    if (cross < 0) DrawTriangle(a, b, c, col);
    else           DrawTriangle(a, c, b, col);
}

static void DrawArrowHead(Vector2 tip, Vector2 dir, Color col) {
    dir = Vector2Normalize(dir);
    Vector2 base = Vector2Subtract(tip, Vector2Scale(dir, ARROW_LEN));
    Vector2 n = Vector2Scale(Perp(dir), ARROW_W);
    FillTriangle(tip, Vector2Add(base, n), Vector2Subtract(base, n), col);
}

static void DrawLabel(const char *text, Vector2 center) {
    Vector2 size = MeasureTextEx(GetFontDefault(), text, FONT_SIZE, 2);
    Vector2 at = { center.x - size.x / 2, center.y - size.y / 2 };
    DrawTextEx(GetFontDefault(), text, at, FONT_SIZE, 2, INK);
}

// Draw a polyline, stopping short so the arrowhead covers the end.
static void DrawPolyArrow(Vector2 *pts, int n) {
    Vector2 tip = pts[n - 1];
    Vector2 dir = Vector2Subtract(tip, pts[n - 2]);
    Vector2 stop = Vector2Subtract(tip, Vector2Scale(Vector2Normalize(dir), ARROW_LEN * 0.8f));
    for (int i = 0; i < n - 2; i++) DrawLineEx(pts[i], pts[i + 1], STROKE, INK);
    DrawLineEx(pts[n - 2], stop, STROKE, INK);
    DrawArrowHead(tip, dir, INK);
}

static bool HasReverse(int e) {
    for (int i = 0; i < n_edges; i++)
        if (edges[i].from == edges[e].to && edges[i].to == edges[e].from) return true;
    return false;
}

// Edge between distinct states: straight, or a circular arc (as in fletcher's
// `bend:`) when there is an edge going the other way.
static void DrawTransition(int e) {
    Vector2 a = states[edges[e].from].pos, b = states[edges[e].to].pos;
    float ra = OuterRadius(edges[e].from), rb = OuterRadius(edges[e].to);
    float d = Vector2Distance(a, b);
    if (d < ra + rb + 1) return;
    Vector2 u = Vector2Scale(Vector2Subtract(b, a), 1.0f / d);
    Vector2 n = Perp(u);

    if (!HasReverse(e)) {
        Vector2 pts[2] = { Vector2Add(a, Vector2Scale(u, ra)), Vector2Subtract(b, Vector2Scale(u, rb)) };
        DrawPolyArrow(pts, 2);
        Vector2 mid = Vector2Lerp(a, b, 0.5f);
        DrawLabel(edges[e].label, Vector2Add(mid, Vector2Scale(n, -14)));
        return;
    }

    // Arc whose tangent at each end makes angle theta with the chord.
    float theta = BEND_DEG * DEG2RAD;
    float r = (d / 2) / sinf(theta);
    Vector2 mid = Vector2Lerp(a, b, 0.5f);
    Vector2 bulge = Vector2Scale(n, -1);  // bulge to the right of travel; reverse edge bulges the other way
    Vector2 c = Vector2Subtract(mid, Vector2Scale(bulge, r * cosf(theta)));
    float a0 = atan2f(a.y - c.y, a.x - c.x);
    float a1 = atan2f(b.y - c.y, b.x - c.x);
    float sweep = a1 - a0;
    while (sweep > PI) sweep -= 2 * PI;
    while (sweep < -PI) sweep += 2 * PI;

    // Sample the arc, keeping only the part outside both node circles.
    Vector2 pts[ARC_SEGS + 1];
    int n_pts = 0;
    for (int i = 0; i <= ARC_SEGS; i++) {
        float t = a0 + sweep * i / ARC_SEGS;
        Vector2 p = { c.x + r * cosf(t), c.y + r * sinf(t) };
        if (Vector2Distance(p, a) >= ra && Vector2Distance(p, b) >= rb) pts[n_pts++] = p;
    }
    if (n_pts < 2) return;
    DrawPolyArrow(pts, n_pts);

    Vector2 apex = Vector2Add(mid, Vector2Scale(bulge, r * (1 - cosf(theta))));
    DrawLabel(edges[e].label, Vector2Add(apex, Vector2Scale(bulge, 14)));
}

static int CompareFloat(const void *a, const void *b) {
    float x = *(const float *)a, y = *(const float *)b;
    return (x > y) - (x < y);
}

// Self-loops point into the widest empty angle between the state's other
// edges (and its initial arrow).
static Vector2 LoopDirection(int s) {
    static float angles[MAX_EDGES * 2 + 1];
    int n = 0;
    for (int i = 0; i < n_edges; i++) {
        int other = -1;
        if (edges[i].from == s && edges[i].to != s) other = edges[i].to;
        if (edges[i].to == s && edges[i].from != s) other = edges[i].from;
        if (other < 0) continue;
        Vector2 d = Vector2Subtract(states[other].pos, states[s].pos);
        angles[n++] = atan2f(d.y, d.x);
    }
    if (states[s].initial) angles[n++] = atan2f(states[s].init_dir.y, states[s].init_dir.x);
    if (n == 0) return (Vector2){ 0, -1 };

    qsort(angles, n, sizeof angles[0], CompareFloat);
    float best_gap = -1, best_mid = 0;
    for (int i = 0; i < n; i++) {
        float next = (i + 1 < n) ? angles[i + 1] : angles[0] + 2 * PI;
        if (next - angles[i] > best_gap) {
            best_gap = next - angles[i];
            best_mid = (angles[i] + next) / 2;
        }
    }
    return (Vector2){ cosf(best_mid), sinf(best_mid) };
}

static void DrawSelfLoop(int e) {
    int s = edges[e].from;
    Vector2 p = states[s].pos;
    float R = OuterRadius(s);
    float lr = NODE_R * 0.55f;           // loop circle radius
    float D = R + lr * 0.45f;            // distance between node and loop centres
    Vector2 dir = LoopDirection(s);
    Vector2 c = Vector2Add(p, Vector2Scale(dir, D));

    // Angle (around the loop centre, measured from the node-facing direction)
    // where the loop circle crosses the node circle.
    float alpha = acosf((D * D + lr * lr - R * R) / (2 * D * lr));
    float back = atan2f(-dir.y, -dir.x);
    Vector2 pts[ARC_SEGS + 1];
    for (int i = 0; i <= ARC_SEGS; i++) {
        float t = back + alpha + (2 * PI - 2 * alpha) * i / ARC_SEGS;
        pts[i] = (Vector2){ c.x + lr * cosf(t), c.y + lr * sinf(t) };
    }
    DrawPolyArrow(pts, ARC_SEGS + 1);
    DrawLabel(edges[e].label, Vector2Add(c, Vector2Scale(dir, lr + 13)));
}

static void DrawState(int s, Color fill) {
    Vector2 p = states[s].pos;
    DrawCircleV(p, OuterRadius(s), fill);
    DrawRing(p, NODE_R - STROKE / 2, NODE_R + STROKE / 2, 0, 360, 64, INK);
    if (states[s].accepting)
        DrawRing(p, NODE_R + ACCEPT_GAP - STROKE / 2, NODE_R + ACCEPT_GAP + STROKE / 2, 0, 360, 64, INK);
    DrawLabel(states[s].label, p);
}

static void DrawInitialArrow(int s) {
    Vector2 p = states[s].pos, dir = states[s].init_dir;
    Vector2 tip = Vector2Add(p, Vector2Scale(dir, OuterRadius(s)));
    Vector2 pts[2] = { Vector2Add(tip, Vector2Scale(dir, INIT_LEN)), tip };
    DrawPolyArrow(pts, 2);
}

static int StateAt(Vector2 m) {
    for (int s = n_states - 1; s >= 0; s--)
        if (Vector2Distance(m, states[s].pos) <= OuterRadius(s)) return s;
    return -1;
}

static Vector2 Snap(Vector2 v) {
    float g = GRID / 2;  // half-grid snapping
    return (Vector2){ roundf(v.x / g) * g, roundf(v.y / g) * g };
}

int main(int argc, char **argv) {
    // --screenshot <file>: render one frame to a PNG and exit.
    const char *shot = NULL, *path = NULL;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--screenshot") == 0 && i + 1 < argc) shot = argv[++i];
        else path = argv[i];
    }
    if (!path) {
        fprintf(stderr, "usage: %s [--screenshot out.png] automaton.dot\n", argv[0]);
        return 2;
    }
    int w, h;
    if (!LoadDot(path, &w, &h)) return 1;

    SetConfigFlags(FLAG_MSAA_4X_HINT | FLAG_WINDOW_RESIZABLE | FLAG_VSYNC_HINT);
    InitWindow(w, h, TextFormat("dfa - %s", GetFileName(path)));
    SetTargetFPS(60);

    int dragging = -1;
    Vector2 grab = { 0 };
    bool snap = false;

    while (!WindowShouldClose()) {
        Vector2 m = GetMousePosition();
        int hover = StateAt(m);

        if (IsMouseButtonPressed(MOUSE_BUTTON_LEFT) && hover >= 0) {
            dragging = hover;
            grab = Vector2Subtract(states[hover].pos, m);
        }
        if (IsMouseButtonReleased(MOUSE_BUTTON_LEFT)) dragging = -1;
        if (dragging >= 0) {
            Vector2 p = Vector2Add(m, grab);
            states[dragging].pos = snap ? Snap(p) : p;
        }
        if (IsKeyPressed(KEY_G)) snap = !snap;
        if (IsKeyPressed(KEY_P)) {
            for (int s = 0; s < n_states; s++)
                printf("%s: (%.2f, %.2f)\n", states[s].name, states[s].pos.x / GRID, states[s].pos.y / GRID);
            printf("\n");
            fflush(stdout);
        }

        SetMouseCursor(dragging >= 0 || hover >= 0 ? MOUSE_CURSOR_POINTING_HAND : MOUSE_CURSOR_DEFAULT);

        BeginDrawing();
        ClearBackground(BG);
        if (snap) {
            for (float x = 0; x < GetScreenWidth(); x += GRID) DrawLineV((Vector2){ x, 0 }, (Vector2){ x, GetScreenHeight() }, GRIDC);
            for (float y = 0; y < GetScreenHeight(); y += GRID) DrawLineV((Vector2){ 0, y }, (Vector2){ GetScreenWidth(), y }, GRIDC);
        }
        for (int e = 0; e < n_edges; e++) {
            if (edges[e].from == edges[e].to) DrawSelfLoop(e);
            else DrawTransition(e);
        }
        for (int s = 0; s < n_states; s++)
            if (states[s].initial) DrawInitialArrow(s);
        for (int s = 0; s < n_states; s++)
            DrawState(s, s == dragging ? ACTIVE : s == hover ? HOVER : BG);

        DrawText(TextFormat("drag states   [G] snap: %s   [P] print positions", snap ? "on" : "off"),
                 12, GetScreenHeight() - 26, 16, GRAY);
        EndDrawing();

        if (shot) { TakeScreenshot(shot); break; }
    }

    CloseWindow();
    return 0;
}
