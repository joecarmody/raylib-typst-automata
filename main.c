// Drag-to-arrange finite automaton viewer.
//
// Usage: dfa [-o out.typ] [--export] [--screenshot out.png] automaton.dot
//
//   -o out.typ   where to write fletcher code (default: automaton.typ)
//   --export     write the fletcher code for Graphviz's layout and exit
//
// The DOT file is laid out by Graphviz (`dot -Tplain`) for the starting
// positions. Conventions:
//   shape=doublecircle                    accepting state
//   edge from a shape=point, style=invis  marks its target as initial
//     or unlabelled shape=none node         (when that is all the node does)
//   any other invisible node              a loose arrow end: shown as a
//                                         dashed handle, exported as a
//                                         coordinate rather than a node
//   several edges between the same pair   merged, labels joined with ", "
//
// Controls:
//   left-drag     move a state or an edge label
//   right-click   reset an edge label to fletcher's default placement
//   G             toggle grid snapping
//   E             export fletcher code
//   C, Esc        enter / leave colour mode
//
// Colour mode:
//   1-7 or click a swatch   pick a fill colour
//   left-click a state      fill it (click again to clear)
//   right-click a state     clear its fill

#define _POSIX_C_SOURCE 200809L  // popen, getline, strdup

#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "raylib.h"
#include "raymath.h"

#define NODE_R      28.0f
#define GHOST_R     9.0f     // handle drawn at a loose arrow end
#define ACCEPT_GAP  4.0f     // inner ring of accepting states
#define GRID        60.0f
#define BEND_DEG    25.0f
#define ARROW_LEN   12.0f
#define ARROW_W     5.0f
#define STROKE      2.0f
#define FONT_SIZE   20
#define ARC_SEGS    48
#define INIT_LEN    40.0f
#define FLETCHER_VERSION "0.5.8"

// Edge labels, measured as fletcher does. The export maps one grid square to
// 1cm, and Typst's default text size is 11pt.
#define PT_PX          (GRID / 28.3465f)
#define EM_PX          (11 * PT_PX)
#define LABEL_SEP_EM   0.4f    // fletcher's default label-sep
#define LABEL_INSET_EM 0.2f    // inset of fletcher's label box
#define LABEL_POS_STEP 0.05f   // dragged label-pos snaps to this
#define LABEL_SEP_STEP 0.1f    // dragged label-sep snaps to this (em)
#define CENTER_BAND    6.0f    // a label dragged this close (px) sits on the edge

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
    int fill;          // index into palette[], or -1 for none
    bool ghost;        // invisible in DOT: a loose arrow end, not a real state
} State;

// Which side of an edge its label sits on, relative to the direction of
// travel, as in fletcher's `label-side`.
typedef enum { SIDE_AUTO, SIDE_LEFT, SIDE_RIGHT, SIDE_CENTER } LabelSide;

typedef struct {
    int from, to;
    char *label;
    float label_pos;       // fraction along the drawn edge
    LabelSide label_side;
    float label_sep;       // em; negative for fletcher's default
} Edge;

typedef struct {
    Vector2 pts[ARC_SEGS + 1];
    int n;
} Path;

static State states[MAX_STATES];
static int n_states;
static Edge edges[MAX_EDGES];
static int n_edges;

static const Color BG     = { 250, 249, 246, 255 };
static const Color INK    = {  30,  30,  35, 255 };
static const Color GRIDC  = { 225, 222, 215, 255 };
static const Color HOVER  = { 230, 238, 250, 255 };
static const Color ACTIVE = { 205, 222, 248, 255 };
static const Color GHOSTC = { 150, 150, 160, 255 };

// State fills: Typst's named colours lightened by FILL_LIGHTEN, with the RGB
// values Typst computes for them.
#define FILL_LIGHTEN "65%"
typedef struct {
    const char *typst;
    Color rgb;
} Paint;

static const Paint palette[] = {
    { "red",    { 0xff, 0xbc, 0xb9, 255 } },
    { "orange", { 0xff, 0xd4, 0xaf, 255 } },
    { "yellow", { 0xff, 0xf3, 0xa6, 255 } },
    { "green",  { 0xb6, 0xed, 0xbc, 255 } },
    { "blue",   { 0xa6, 0xce, 0xf2, 255 } },
    { "purple", { 0xe4, 0xaa, 0xec, 255 } },
    { "gray",   { 0xe1, 0xe1, 0xe1, 255 } },
};
#define N_PAINTS ((int)(sizeof palette / sizeof palette[0]))
#define SWATCH   22

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
    bool invisible;
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
    edges[n_edges++] = (Edge){ from, to, strdup(label), 0.5f, SIDE_AUTO, -1 };
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
            bool invisible = strcmp(shape, "point") == 0 || strstr(style, "invis") ||
                             ((strcmp(shape, "none") == 0 || strcmp(shape, "plaintext") == 0) && !*label);
            nodes[n_nodes++] = (DotNode){
                .name = strdup(tok[1]), .label = strdup(label),
                .pos = { atof(tok[2]), atof(tok[3]) },
                .invisible = invisible, .marker = invisible, .accepting = strcmp(shape, "doublecircle") == 0,
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

    // An invisible node whose edges are all unlabelled and outgoing marks
    // initial states. Any other invisible node is a loose arrow end (ghost).
    for (int i = 0; i < n_dedges; i++) {
        DotNode *tail = &nodes[dedges[i].tail], *head = &nodes[dedges[i].head];
        head->marker = false;
        if (*dedges[i].label || head->invisible) tail->marker = false;
    }
    for (int i = 0; i < n_dedges; i++)
        if (nodes[dedges[i].tail].marker)
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
            .name = dn->name, .label = dn->label, .accepting = dn->accepting, .fill = -1,
            .ghost = dn->invisible,
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

// Labels written as Typst math ($...$) are shown without the dollars.
static const char *DisplayText(const char *text) {
    size_t len = strlen(text);
    if (len >= 2 && text[0] == '$' && text[len - 1] == '$') return TextSubtext(text, 1, len - 2);
    return text;
}

static void DrawLabel(const char *text, Vector2 center) {
    text = DisplayText(text);
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

// How far an edge stops short of a state's centre.
static float EndRadius(int s) {
    return states[s].ghost ? 0 : NODE_R;
}

// The edge as drawn, from the source's boundary to the arrow tip: a straight
// line, a circular arc (as in fletcher's `bend:`) when there is an edge going
// the other way, or a self-loop. Empty if the states overlap.
static Path EdgePath(int e) {
    Path path = { .n = 0 };
    Vector2 a = states[edges[e].from].pos, b = states[edges[e].to].pos;

    float ra = EndRadius(edges[e].from), rb = EndRadius(edges[e].to);

    if (edges[e].from == edges[e].to) {
        if (states[edges[e].from].ghost) return path;
        float lr = NODE_R * 0.55f;           // loop circle radius
        float D = NODE_R + lr * 0.45f;       // distance between node and loop centres
        Vector2 dir = LoopDirection(edges[e].from);
        Vector2 c = Vector2Add(a, Vector2Scale(dir, D));

        // Angle (around the loop centre, measured from the node-facing direction)
        // where the loop circle crosses the node circle.
        float alpha = acosf((D * D + lr * lr - NODE_R * NODE_R) / (2 * D * lr));
        float back = atan2f(-dir.y, -dir.x);
        for (int i = 0; i <= ARC_SEGS; i++) {
            float t = back + alpha + (2 * PI - 2 * alpha) * i / ARC_SEGS;
            path.pts[path.n++] = (Vector2){ c.x + lr * cosf(t), c.y + lr * sinf(t) };
        }
        return path;
    }

    float d = Vector2Distance(a, b);
    if (d < ra + rb + 1) return path;
    Vector2 u = Vector2Scale(Vector2Subtract(b, a), 1.0f / d);

    if (!HasReverse(e)) {
        path.pts[0] = Vector2Add(a, Vector2Scale(u, ra));
        path.pts[1] = Vector2Subtract(b, Vector2Scale(u, rb));
        path.n = 2;
        return path;
    }

    // Arc whose tangent at each end makes angle theta with the chord, bulging
    // to the left of travel (so the reverse edge bulges the other way).
    float theta = BEND_DEG * DEG2RAD;
    float r = (d / 2) / sinf(theta);
    Vector2 left = { u.y, -u.x };
    Vector2 c = Vector2Subtract(Vector2Lerp(a, b, 0.5f), Vector2Scale(left, r * cosf(theta)));
    float a0 = atan2f(a.y - c.y, a.x - c.x);
    float a1 = atan2f(b.y - c.y, b.x - c.x);
    float sweep = a1 - a0;
    while (sweep > PI) sweep -= 2 * PI;
    while (sweep < -PI) sweep += 2 * PI;

    // Sample the arc, keeping only the part outside both node circles.
    for (int i = 0; i <= ARC_SEGS; i++) {
        float t = a0 + sweep * i / ARC_SEGS;
        Vector2 p = { c.x + r * cosf(t), c.y + r * sinf(t) };
        if (Vector2Distance(p, a) >= ra && Vector2Distance(p, b) >= rb) path.pts[path.n++] = p;
    }
    if (path.n < 2) path.n = 0;
    return path;
}

static float PathLength(const Path *p) {
    float len = 0;
    for (int i = 0; i + 1 < p->n; i++) len += Vector2Distance(p->pts[i], p->pts[i + 1]);
    return len;
}

// Point at fraction t of the path's length, and the unit tangent there.
static Vector2 PathAt(const Path *p, float t, Vector2 *tangent) {
    float target = t * PathLength(p);
    for (int i = 0; i + 1 < p->n; i++) {
        float seg = Vector2Distance(p->pts[i], p->pts[i + 1]);
        if (target <= seg || i + 2 == p->n) {
            *tangent = Vector2Normalize(Vector2Subtract(p->pts[i + 1], p->pts[i]));
            return Vector2Lerp(p->pts[i], p->pts[i + 1], seg > 0 ? fminf(target / seg, 1) : 0);
        }
        target -= seg;
    }
    *tangent = (Vector2){ 1, 0 };
    return p->pts[0];
}

// Fraction along the path of the point nearest q. *left gets q's distance
// from the path, positive to the left of travel.
static float PathNearest(const Path *p, Vector2 q, float *left) {
    float total = PathLength(p), walked = 0, best = INFINITY, best_t = 0;
    *left = 0;
    for (int i = 0; i + 1 < p->n; i++) {
        Vector2 a = p->pts[i], ab = Vector2Subtract(p->pts[i + 1], a);
        float seg = Vector2Length(ab);
        float k = seg > 0 ? Clamp(Vector2DotProduct(Vector2Subtract(q, a), ab) / (seg * seg), 0, 1) : 0;
        Vector2 near = Vector2Add(a, Vector2Scale(ab, k));
        float dist = Vector2Distance(q, near);
        if (dist < best) {
            Vector2 lhs = { ab.y, -ab.x };
            best = dist;
            best_t = total > 0 ? (walked + k * seg) / total : 0;
            *left = Vector2DotProduct(Vector2Subtract(q, near), lhs) < 0 ? -dist : dist;
        }
        walked += seg;
    }
    return best_t;
}

// fletcher's `label-side: auto`: outside the curve for bent edges and loops
// (both bend left); for straight edges, left when heading rightwards and
// otherwise right, so the label ends up roughly above.
static LabelSide AutoSide(int e) {
    if (edges[e].from == edges[e].to || HasReverse(e)) return SIDE_LEFT;
    Vector2 d = Vector2Subtract(states[edges[e].to].pos, states[edges[e].from].pos);
    return fabsf(atan2f(-d.y, d.x)) < PI / 2 ? SIDE_LEFT : SIDE_RIGHT;
}

static LabelSide ResolvedSide(int e) {
    return edges[e].label_side == SIDE_AUTO ? AutoSide(e) : edges[e].label_side;
}

// Unit normal pointing from the edge towards a label on the given side.
static Vector2 SideNormal(Vector2 tangent, LabelSide side) {
    Vector2 left = { tangent.y, -tangent.x };
    return side == SIDE_RIGHT ? Vector2Negate(left) : left;
}

// Size of fletcher's label box: the text plus its inset. Typst's text box
// spans cap height to baseline, roughly 0.7em.
static Vector2 LabelSize(const char *text) {
    float w = MeasureTextEx(GetFontDefault(), DisplayText(text), FONT_SIZE, 2).x;
    return (Vector2){ w + 2 * LABEL_INSET_EM * EM_PX, (0.7f + 2 * LABEL_INSET_EM) * EM_PX };
}

// Offset from a label box's centre to its anchor: the compass point facing
// `toward`, rounded to 45 degrees as in fletcher's angle-to-anchor.
static Vector2 AnchorOffset(Vector2 toward, Vector2 size) {
    float a = roundf(atan2f(toward.y, toward.x) / (PI / 4)) * (PI / 4);
    return (Vector2){ roundf(cosf(a)) * size.x / 2, roundf(sinf(a)) * size.y / 2 };
}

static Rectangle LabelRect(int e, const Path *p) {
    Vector2 size = LabelSize(edges[e].label), tangent;
    Vector2 at = PathAt(p, edges[e].label_pos, &tangent);
    LabelSide side = ResolvedSide(e);
    Vector2 c = at;
    if (side != SIDE_CENTER) {
        Vector2 out = SideNormal(tangent, side);
        float sep = (edges[e].label_sep < 0 ? LABEL_SEP_EM : edges[e].label_sep) * EM_PX;
        Vector2 anchor = Vector2Add(at, Vector2Scale(out, sep));
        c = Vector2Subtract(anchor, AnchorOffset(Vector2Negate(out), size));
    }
    return (Rectangle){ c.x - size.x / 2, c.y - size.y / 2, size.x, size.y };
}

static Vector2 RectCenter(Rectangle r) {
    return (Vector2){ r.x + r.width / 2, r.y + r.height / 2 };
}

// Place edge e's label as near as fletcher allows to a box centred at c.
static void DragLabel(int e, Vector2 c) {
    Path p = EdgePath(e);
    if (p.n < 2) return;
    float left;
    float t = PathNearest(&p, c, &left);
    t = Clamp(roundf(t / LABEL_POS_STEP) * LABEL_POS_STEP, 0, 1);
    edges[e].label_pos = t;

    if (fabsf(left) < CENTER_BAND) {
        edges[e].label_side = SIDE_CENTER;
        return;
    }
    LabelSide side = left > 0 ? SIDE_LEFT : SIDE_RIGHT;
    Vector2 tangent;
    Vector2 at = PathAt(&p, t, &tangent);
    Vector2 out = SideNormal(tangent, side);
    Vector2 anchor = Vector2Add(c, AnchorOffset(Vector2Negate(out), LabelSize(edges[e].label)));
    float sep = fmaxf(0, Vector2DotProduct(Vector2Subtract(anchor, at), out) / EM_PX);
    sep = roundf(sep / LABEL_SEP_STEP) * LABEL_SEP_STEP;

    edges[e].label_side = side == AutoSide(e) ? SIDE_AUTO : side;
    edges[e].label_sep = fabsf(sep - LABEL_SEP_EM) < LABEL_SEP_STEP / 2 ? -1 : sep;
}

// fill: highlight behind the label, or BLANK for none.
static void DrawEdge(int e, Color fill) {
    Path p = EdgePath(e);
    if (p.n < 2) return;
    DrawPolyArrow(p.pts, p.n);
    if (!*edges[e].label) return;
    Rectangle r = LabelRect(e, &p);
    if (fill.a > 0) DrawRectangleRounded(r, 0.3f, 4, fill);
    else if (ResolvedSide(e) == SIDE_CENTER) DrawRectangleRec(r, BG);  // fletcher fills centred labels
    DrawLabel(edges[e].label, RectCenter(r));
}

static int LabelAt(Vector2 m) {
    for (int e = n_edges - 1; e >= 0; e--) {
        if (!*edges[e].label) continue;
        Path p = EdgePath(e);
        if (p.n >= 2 && CheckCollisionPointRec(m, LabelRect(e, &p))) return e;
    }
    return -1;
}

static void DrawState(int s, Color fill) {
    Vector2 p = states[s].pos;
    DrawCircleV(p, NODE_R, fill);
    DrawRing(p, NODE_R - STROKE / 2, NODE_R + STROKE / 2, 0, 360, 64, INK);
    if (states[s].accepting)
        DrawRing(p, NODE_R - ACCEPT_GAP - STROKE / 2, NODE_R - ACCEPT_GAP + STROKE / 2, 0, 360, 64, INK);
    DrawLabel(states[s].label, p);
}

// Dashed handle at a loose arrow end. Editor only: nothing is exported for it.
static void DrawGhost(int s, Color fill) {
    Vector2 p = states[s].pos;
    if (fill.a > 0) DrawCircleV(p, GHOST_R, Fade(fill, 0.8f));
    for (int a = 0; a < 360; a += 30)
        DrawRing(p, GHOST_R - STROKE / 2, GHOST_R + STROKE / 2, a, a + 15, 4, GHOSTC);
}

static void DrawInitialArrow(int s) {
    Vector2 p = states[s].pos, dir = states[s].init_dir;
    Vector2 tip = Vector2Add(p, Vector2Scale(dir, NODE_R));
    Vector2 pts[2] = { Vector2Add(tip, Vector2Scale(dir, INIT_LEN)), tip };
    DrawPolyArrow(pts, 2);
}

static Rectangle SwatchRect(int i) {
    return (Rectangle){ GetScreenWidth() - (N_PAINTS - i) * (SWATCH + 6) - 6,
                        GetScreenHeight() - SWATCH - 8, SWATCH, SWATCH };
}

static void DrawPalette(int brush) {
    for (int i = 0; i < N_PAINTS; i++) {
        Rectangle r = SwatchRect(i);
        DrawRectangleRec(r, palette[i].rgb);
        DrawRectangleLinesEx(r, i == brush ? 3 : 1, i == brush ? INK : GRAY);
        DrawText(TextFormat("%d", i + 1), r.x + 7, r.y + 5, 14, INK);
    }
}

static int SwatchAt(Vector2 m) {
    for (int i = 0; i < N_PAINTS; i++)
        if (CheckCollisionPointRec(m, SwatchRect(i))) return i;
    return -1;
}

static int StateAt(Vector2 m) {
    for (int s = n_states - 1; s >= 0; s--)
        if (Vector2Distance(m, states[s].pos) <= (states[s].ghost ? GHOST_R + 3 : NODE_R)) return s;
    return -1;
}

static Vector2 Snap(Vector2 v) {
    float g = GRID / 2;  // half-grid snapping
    return (Vector2){ roundf(v.x / g) * g, roundf(v.y / g) * g };
}

// ---------------------------------------------------------------------------
// fletcher export

// Grid coordinate with trailing zeros dropped: 1.5, 0.25, 2. Returns one of a
// few rotating static buffers so several can appear in one printf.
static const char *Num(float v) {
    static char buf[8][32];
    static int next;
    char *b = buf[next++ % 8];
    snprintf(b, sizeof buf[0], "%.3f", v);
    char *end = b + strlen(b) - 1;
    while (*end == '0') *end-- = '\0';
    if (*end == '.') *end = '\0';
    if (strcmp(b, "-0") == 0) strcpy(b, "0");
    return b;
}

static void WriteTypstString(FILE *f, const char *s) {
    fputc('"', f);
    for (; *s; s++) {
        if (*s == '"' || *s == '\\') fputc('\\', f);
        fputc(*s, f);
    }
    fputc('"', f);
}

// State label content: $...$ passes through as math; plain text is written
// as markup when it can't be mistaken for markup, otherwise as a string.
static void WriteText(FILE *f, const char *s) {
    size_t len = strlen(s);
    if (len >= 2 && s[0] == '$' && s[len - 1] == '$') { fputs(s, f); return; }
    bool plain = true;
    for (const char *c = s; *c; c++)
        if (!isalnum((unsigned char)*c) && !strchr(" ,:;()!?|&%", *c)) plain = false;
    if (plain) {
        fprintf(f, "[%s]", s);
    } else {
        fputs("[#", f);
        WriteTypstString(f, s);
        fputc(']', f);
    }
}

// Edge label content, typeset as math. $...$ passes through as Typst math
// source; anything else is taken literally: punctuation is escaped (so `->`
// or `_` stay as typed), and letters are spaced apart from neighbouring
// letters and digits so runs like `ab` or `q0` aren't read as variable names.
// Non-ASCII characters (say `ε`) are spaced like letters.
static void WriteMath(FILE *f, const char *s) {
    size_t len = strlen(s);
    if (len >= 2 && s[0] == '$' && s[len - 1] == '$') { fputs(s, f); return; }
    if (len == 0) { fputs("[]", f); return; }
    fputc('$', f);
    unsigned char prev = 0;  // previous character's first byte
    for (const unsigned char *c = (const unsigned char *)s; *c; c++) {
        if ((*c & 0xC0) == 0x80) { fputc(*c, f); continue; }  // rest of a UTF-8 character
        bool word = isalnum(*c) || *c >= 0x80, prev_word = isalnum(prev) || prev >= 0x80;
        if (word && prev_word && !(isdigit(*c) && isdigit(prev))) fputc(' ', f);
        if (ispunct(*c)) fputc('\\', f);
        fputc(*c, f);
        prev = *c;
    }
    fputc('$', f);
}

// State names become Typst labels: <q0> when valid, else label("...").
static void WriteName(FILE *f, const char *s) {
    bool simple = *s != '\0';
    for (const char *c = s; *c; c++)
        if (!isalnum((unsigned char)*c) && !strchr("_-:.", *c)) simple = false;
    if (simple) {
        fprintf(f, "<%s>", s);
    } else {
        fputs("label(", f);
        WriteTypstString(f, s);
        fputc(')', f);
    }
}

// An edge end: the state's name, or the bare coordinate of a loose end.
static void WriteEndpoint(FILE *f, int s, Vector2 origin) {
    if (states[s].ghost) {
        Vector2 g = Vector2Scale(Vector2Subtract(states[s].pos, origin), 1 / GRID);
        fprintf(f, "at(%s, %s)", Num(g.x), Num(g.y));
    } else {
        WriteName(f, states[s].name);
    }
}

static bool ExportFletcher(const char *out, const char *source) {
    FILE *f = fopen(out, "w");
    if (!f) { perror(out); return false; }

    // Grid coordinates relative to the top-left-most state, y down.
    Vector2 origin = { INFINITY, INFINITY };
    for (int s = 0; s < n_states; s++) {
        if (states[s].ghost) continue;
        origin.x = fminf(origin.x, states[s].pos.x);
        origin.y = fminf(origin.y, states[s].pos.y);
    }
    if (isinf(origin.x)) origin = states[0].pos;

    fprintf(f, "// Generated by dfa from %s; exporting again overwrites this file.\n", GetFileName(source));
    fprintf(f, "#import \"@preview/fletcher:%s\" as fletcher: diagram, node, edge\n\n", FLETCHER_VERSION);
    fprintf(f, "#diagram(\n  node-stroke: 0.8pt,\n  {\n");
    fprintf(f, "    let u = 1cm  // one editor grid square\n");
    fprintf(f, "    let at(x, y) = (x * u, -y * u)  // y points down, as in the editor\n");
    fprintf(f, "    let state = node.with(shape: circle, radius: %s * u)\n\n", Num(NODE_R / GRID));

    for (int s = 0; s < n_states; s++) {
        if (states[s].ghost) continue;
        Vector2 g = Vector2Scale(Vector2Subtract(states[s].pos, origin), 1 / GRID);
        fprintf(f, "    state(at(%s, %s), ", Num(g.x), Num(g.y));
        WriteText(f, states[s].label);
        fputs(", name: ", f);
        WriteName(f, states[s].name);
        // Outer stroke first: fletcher fills inside the first extrude offset.
        if (states[s].accepting) fputs(", extrude: (0, -2.5)", f);
        if (states[s].fill >= 0) fprintf(f, ", fill: %s.lighten(%s)", palette[states[s].fill].typst, FILL_LIGHTEN);
        fputs(")\n", f);
    }
    fputc('\n', f);

    for (int s = 0; s < n_states; s++) {
        if (!states[s].initial) continue;
        Vector2 d = Vector2Scale(states[s].init_dir, (NODE_R + INIT_LEN) / GRID);
        fprintf(f, "    edge((rel: at(%s, %s), to: ", Num(d.x), Num(d.y));
        WriteName(f, states[s].name);
        fputs("), ", f);
        WriteName(f, states[s].name);
        fputs(", \"-|>\")\n", f);
    }

    for (int e = 0; e < n_edges; e++) {
        fputs("    edge(", f);
        WriteEndpoint(f, edges[e].from, origin);
        fputs(", ", f);
        WriteEndpoint(f, edges[e].to, origin);
        fputs(", \"-|>\"", f);
        if (*edges[e].label) {
            static const char *side_names[] = { "auto", "left", "right", "center" };
            fputs(", ", f);
            WriteMath(f, edges[e].label);
            if (edges[e].label_pos != 0.5f) fprintf(f, ", label-pos: %s", Num(edges[e].label_pos));
            if (edges[e].label_side != SIDE_AUTO) fprintf(f, ", label-side: %s", side_names[edges[e].label_side]);
            if (edges[e].label_sep >= 0 && edges[e].label_side != SIDE_CENTER)
                fprintf(f, ", label-sep: %sem", Num(edges[e].label_sep));
        }
        if (edges[e].from == edges[e].to) {
            Vector2 dir = LoopDirection(edges[e].from);
            fprintf(f, ", bend: 130deg, loop-angle: %.0fdeg", atan2f(-dir.y, dir.x) * RAD2DEG);
        } else if (HasReverse(e)) {
            // Positive fletcher bends bulge to the left of travel, like ours.
            fprintf(f, ", bend: %sdeg", Num(BEND_DEG));
        }
        fputs(")\n", f);
    }
    fprintf(f, "  },\n)\n");

    if (fclose(f) != 0) { perror(out); return false; }
    return true;
}

// automaton.dot -> automaton.typ
static char *DefaultOutput(const char *path) {
    char *out = malloc(strlen(path) + 5);
    strcpy(out, path);
    char *dot = strrchr(out, '.');
    if (dot && (strcmp(dot, ".dot") == 0 || strcmp(dot, ".gv") == 0)) *dot = '\0';
    strcat(out, ".typ");
    return out;
}

int main(int argc, char **argv) {
    // --screenshot <file>: render one frame to a PNG and exit.
    const char *shot = NULL, *path = NULL, *out = NULL;
    bool export_only = false;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--screenshot") == 0 && i + 1 < argc) shot = argv[++i];
        else if (strcmp(argv[i], "-o") == 0 && i + 1 < argc) out = argv[++i];
        else if (strcmp(argv[i], "--export") == 0) export_only = true;
        else path = argv[i];
    }
    if (!path) {
        fprintf(stderr, "usage: %s [-o out.typ] [--export] [--screenshot out.png] automaton.dot\n", argv[0]);
        return 2;
    }
    if (!out) out = DefaultOutput(path);
    int w, h;
    if (!LoadDot(path, &w, &h)) return 1;
    if (export_only) return ExportFletcher(out, path) ? 0 : 1;

    SetConfigFlags(FLAG_MSAA_4X_HINT | FLAG_WINDOW_RESIZABLE | FLAG_VSYNC_HINT);
    InitWindow(w, h, TextFormat("dfa - %s", GetFileName(path)));
    SetTargetFPS(60);

    int dragging = -1, dragging_label = -1;
    Vector2 grab = { 0 };
    bool snap = false;
    bool colour_mode = false;
    int brush = 0;
    char status[512] = "";
    double status_until = 0;

    while (!WindowShouldClose()) {
        Vector2 m = GetMousePosition();
        int hover = StateAt(m);
        int hover_label = hover < 0 && !colour_mode ? LabelAt(m) : -1;
        int hover_swatch = colour_mode ? SwatchAt(m) : -1;

        if (IsKeyPressed(KEY_C)) colour_mode = !colour_mode;
        if (colour_mode && IsKeyPressed(KEY_ESCAPE)) colour_mode = false;
        for (int i = 0; i < N_PAINTS; i++)
            if (IsKeyPressed(KEY_ONE + i)) brush = i, colour_mode = true;
        // Esc leaves colour mode rather than quitting.
        SetExitKey(colour_mode ? KEY_NULL : KEY_ESCAPE);

        if (colour_mode) {
            if (IsMouseButtonPressed(MOUSE_BUTTON_LEFT) && hover_swatch >= 0) brush = hover_swatch;
            bool paintable = hover >= 0 && !states[hover].ghost;
            if (IsMouseButtonPressed(MOUSE_BUTTON_LEFT) && paintable)
                states[hover].fill = states[hover].fill == brush ? -1 : brush;
            if (IsMouseButtonPressed(MOUSE_BUTTON_RIGHT) && paintable) states[hover].fill = -1;
        } else if (IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) {
            if (hover >= 0) {
                dragging = hover;
                grab = Vector2Subtract(states[hover].pos, m);
            } else if (hover_label >= 0) {
                Path p = EdgePath(hover_label);
                dragging_label = hover_label;
                grab = Vector2Subtract(RectCenter(LabelRect(hover_label, &p)), m);
            }
        }
        if (IsMouseButtonReleased(MOUSE_BUTTON_LEFT)) dragging = dragging_label = -1;
        if (dragging >= 0) {
            Vector2 p = Vector2Add(m, grab);
            states[dragging].pos = snap ? Snap(p) : p;
        }
        if (dragging_label >= 0) DragLabel(dragging_label, Vector2Add(m, grab));
        if (IsMouseButtonPressed(MOUSE_BUTTON_RIGHT) && hover_label >= 0) {
            edges[hover_label].label_pos = 0.5f;
            edges[hover_label].label_side = SIDE_AUTO;
            edges[hover_label].label_sep = -1;
        }
        if (IsKeyPressed(KEY_G)) snap = !snap;
        if (IsKeyPressed(KEY_E)) {
            bool ok = ExportFletcher(out, path);
            snprintf(status, sizeof status, ok ? "exported to %s" : "could not write %s", out);
            status_until = GetTime() + 3;
            if (ok) printf("dfa: %s\n", status);
        }

        bool pointing = dragging >= 0 || dragging_label >= 0 || hover >= 0 || hover_label >= 0 || hover_swatch >= 0;
        SetMouseCursor(pointing ? MOUSE_CURSOR_POINTING_HAND : MOUSE_CURSOR_DEFAULT);

        BeginDrawing();
        ClearBackground(BG);
        if (snap) {
            for (float x = 0; x < GetScreenWidth(); x += GRID) DrawLineV((Vector2){ x, 0 }, (Vector2){ x, GetScreenHeight() }, GRIDC);
            for (float y = 0; y < GetScreenHeight(); y += GRID) DrawLineV((Vector2){ 0, y }, (Vector2){ GetScreenWidth(), y }, GRIDC);
        }
        for (int e = 0; e < n_edges; e++)
            DrawEdge(e, e == dragging_label ? ACTIVE : e == hover_label ? HOVER : BLANK);
        for (int s = 0; s < n_states; s++)
            if (states[s].initial) DrawInitialArrow(s);
        for (int s = 0; s < n_states; s++) {
            if (states[s].ghost) {
                bool live = !colour_mode && (s == dragging || s == hover);
                DrawGhost(s, live ? (s == dragging ? ACTIVE : HOVER) : BLANK);
                continue;
            }
            Color fill = states[s].fill >= 0 ? palette[states[s].fill].rgb : BG;
            if (colour_mode && s == hover) {
                // Preview what a click would do.
                fill = states[s].fill == brush ? BG : palette[brush].rgb;
            } else if (s == dragging || s == hover) {
                Color tint = s == dragging ? ACTIVE : HOVER;
                fill = states[s].fill >= 0 ? ColorBrightness(fill, s == dragging ? -0.15f : -0.08f) : tint;
            }
            DrawState(s, fill);
        }

        const char *help = colour_mode
            ? "colour mode: click a state to fill it   [1-7] pick colour   [C/Esc] done"
            : TextFormat("drag states and labels   [G] snap: %s   [C] colour   [E] export fletcher", snap ? "on" : "off");
        DrawText(GetTime() < status_until ? status : help, 12, GetScreenHeight() - 26, 16, GRAY);
        if (colour_mode) DrawPalette(brush);
        EndDrawing();

        if (shot) { TakeScreenshot(shot); break; }
    }

    CloseWindow();
    return 0;
}
