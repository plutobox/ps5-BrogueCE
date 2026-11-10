/* ps5main.c - PS5 entry point, display and DualSense input for Brogue CE.
 *
 * The native title (PS5_NATIVE, eboot.bin).  ps5-opengl's SDL2 has only an
 * OpenGL window: no window surface, and no render backend but the software
 * one.  Brogue's own tile renderer (src/platform/tiles.c) is kept as it is:
 * it asks SDL for a window and a renderer, and the calls below, bound with
 * --wrap, hand it the OpenGL window and a software renderer that draws into
 * an off-screen surface.  Each finished frame is uploaded and shown with
 * OpenGL, with a bar of button hints under the dungeon.
 *
 * The game's data is read-only at the title root, /app0 (assets/).  Saves,
 * recordings, scores and settings go under /download0/brogue (ps5_io.h
 * redirects Brogue's relative paths there).  A title may not exit, so exit()
 * waits for the player to close the title from the home screen.
 */
#include <SDL.h>
#include <GL/glcorearb.h>
#include <SDL_image.h>
#include <errno.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "ps5_io.h"

#define STORAGE "/download0/brogue"

extern int brogue_main(int argc, char **argv);

/* sdl2-platform.c: what the player is being asked for (enum padContexts in
 * Rogue.h: the map, the keyboard cursor, aiming, a menu of buttons, a
 * message to dismiss, a line of text). */
extern int ps5HintContext(void);
enum {
    CONTEXT_MAP, CONTEXT_CURSOR, CONTEXT_AIM, CONTEXT_BUTTONS,
    CONTEXT_ACKNOWLEDGE, CONTEXT_TEXT, CONTEXT_COUNT
};

/* Brogue's own key codes (Rogue.h), for the keys that are not characters. */
#define PAD_MENU_KEY 63290

/* ---- Paths: relative file names go to the writable storage -------------- */

static const char *storage_path(const char *path, char *buffer, size_t size)
{
    int length;

    if (!path || path[0] == '/')
        return path;
    if (!strcmp(path, ".") || !strcmp(path, "./"))
        return STORAGE;
    length = snprintf(buffer, size, STORAGE "/%s", path);
    return (length > 0 && (size_t) length < size) ? buffer : path;
}

FILE *ps5io_fopen(const char *path, const char *mode)
{
    char buffer[1024];

    return (fopen)(storage_path(path, buffer, sizeof buffer), mode);
}

int ps5io_remove(const char *path)
{
    char buffer[1024];

    return (remove)(storage_path(path, buffer, sizeof buffer));
}

int ps5io_rename(const char *from, const char *to)
{
    char a[1024], b[1024];

    return (rename)(storage_path(from, a, sizeof a),
                    storage_path(to, b, sizeof b));
}

int ps5io_stat(const char *path, struct stat *sb)
{
    char buffer[1024];

    return (stat)(storage_path(path, buffer, sizeof buffer), sb);
}

DIR *ps5io_opendir(const char *path)
{
    char buffer[1024];

    return (opendir)(storage_path(path, buffer, sizeof buffer));
}

/* ---- Console services ---------------------------------------------------- */

int sceKernelUsleep(unsigned int microseconds);
int sceUserServiceInitialize(void *params);
int sceUserServiceGetForegroundUser(int *user_id);

/* ---- The OpenGL presenter ----------------------------------------------- */

SDL_Window *__real_SDL_CreateWindow(const char *title, int x, int y, int w,
                                    int h, Uint32 flags);

static SDL_Window *window;
static SDL_Surface *screen;     /* the whole frame: the dungeon, then the hints */
static SDL_Surface *game;       /* the top of `screen`, which Brogue draws in */
static SDL_Renderer *renderer;  /* a software renderer drawing into `game` */
static GLuint texture, program, vertex_array;
static int hint_top;            /* first row of the hint bar */
static int game_dirty;          /* Brogue has drawn since the last upload */

static PFNGLGENTEXTURESPROC gl_GenTextures;
static PFNGLBINDTEXTUREPROC gl_BindTexture;
static PFNGLTEXPARAMETERIPROC gl_TexParameteri;
static PFNGLTEXIMAGE2DPROC gl_TexImage2D;
static PFNGLTEXSUBIMAGE2DPROC gl_TexSubImage2D;
static PFNGLPIXELSTOREIPROC gl_PixelStorei;
static PFNGLCREATESHADERPROC gl_CreateShader;
static PFNGLSHADERSOURCEPROC gl_ShaderSource;
static PFNGLCOMPILESHADERPROC gl_CompileShader;
static PFNGLGETSHADERIVPROC gl_GetShaderiv;
static PFNGLGETSHADERINFOLOGPROC gl_GetShaderInfoLog;
static PFNGLCREATEPROGRAMPROC gl_CreateProgram;
static PFNGLATTACHSHADERPROC gl_AttachShader;
static PFNGLLINKPROGRAMPROC gl_LinkProgram;
static PFNGLGETPROGRAMIVPROC gl_GetProgramiv;
static PFNGLUSEPROGRAMPROC gl_UseProgram;
static PFNGLGETUNIFORMLOCATIONPROC gl_GetUniformLocation;
static PFNGLUNIFORM1IPROC gl_Uniform1i;
static PFNGLGENVERTEXARRAYSPROC gl_GenVertexArrays;
static PFNGLBINDVERTEXARRAYPROC gl_BindVertexArray;
static PFNGLDRAWARRAYSPROC gl_DrawArrays;
static PFNGLVIEWPORTPROC gl_Viewport;
static PFNGLACTIVETEXTUREPROC gl_ActiveTexture;

#define GL_LOAD(var, name) \
    ((var = (void *) SDL_GL_GetProcAddress(name)) != NULL)

/* A triangle covering the screen; row 0 of the surface is the top. */
static const char *vertex_source =
    "#version 330 core\n"
    "out vec2 uv;\n"
    "void main() {\n"
    "    vec2 p = vec2(float((gl_VertexID << 1) & 2), float(gl_VertexID & 2));\n"
    "    uv = vec2(p.x, 1.0 - p.y);\n"
    "    gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0);\n"
    "}\n";
static const char *fragment_source =
    "#version 330 core\n"
    "in vec2 uv;\n"
    "uniform sampler2D surface;\n"
    "out vec4 colour;\n"
    "void main() { colour = vec4(texture(surface, uv).rgb, 1.0); }\n";

static GLuint compile_shader(GLenum type, const char *source)
{
    GLuint shader = gl_CreateShader(type);
    GLint ok = 0;

    gl_ShaderSource(shader, 1, &source, NULL);
    gl_CompileShader(shader);
    gl_GetShaderiv(shader, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[512] = "";

        gl_GetShaderInfoLog(shader, sizeof log, NULL, log);
        printf("brogue: shader compile failed: %s\n", log);
    }
    return shader;
}

/* The hint bar's height: 56 pixels on a 1080-line screen. */
#define HINT_BAR_HEIGHT(h) (((h) * 56 + 540) / 1080)

static int setup_presenter(void)
{
    SDL_DisplayMode mode;
    SDL_GLContext context;
    GLint linked = 0;
    int width, height;

    if (SDL_GetDesktopDisplayMode(0, &mode) < 0)
        return -1;
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK,
                        SDL_GL_CONTEXT_PROFILE_CORE);
    SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
    /* ps5-opengl's driver supports one fixed window the size of the output.
     * Brogue's own SDL_CreateWindow is wrapped to return it. */
    window = __real_SDL_CreateWindow("Brogue", SDL_WINDOWPOS_UNDEFINED,
                                     SDL_WINDOWPOS_UNDEFINED, mode.w, mode.h,
                                     SDL_WINDOW_OPENGL);
    if (!window || !(context = SDL_GL_CreateContext(window))
        || SDL_GL_MakeCurrent(window, context) < 0)
        return -1;
    SDL_GL_SetSwapInterval(1);

    if (!GL_LOAD(gl_GenTextures, "glGenTextures")
        || !GL_LOAD(gl_BindTexture, "glBindTexture")
        || !GL_LOAD(gl_TexParameteri, "glTexParameteri")
        || !GL_LOAD(gl_TexImage2D, "glTexImage2D")
        || !GL_LOAD(gl_TexSubImage2D, "glTexSubImage2D")
        || !GL_LOAD(gl_PixelStorei, "glPixelStorei")
        || !GL_LOAD(gl_CreateShader, "glCreateShader")
        || !GL_LOAD(gl_ShaderSource, "glShaderSource")
        || !GL_LOAD(gl_CompileShader, "glCompileShader")
        || !GL_LOAD(gl_GetShaderiv, "glGetShaderiv")
        || !GL_LOAD(gl_GetShaderInfoLog, "glGetShaderInfoLog")
        || !GL_LOAD(gl_CreateProgram, "glCreateProgram")
        || !GL_LOAD(gl_AttachShader, "glAttachShader")
        || !GL_LOAD(gl_LinkProgram, "glLinkProgram")
        || !GL_LOAD(gl_GetProgramiv, "glGetProgramiv")
        || !GL_LOAD(gl_UseProgram, "glUseProgram")
        || !GL_LOAD(gl_GetUniformLocation, "glGetUniformLocation")
        || !GL_LOAD(gl_Uniform1i, "glUniform1i")
        || !GL_LOAD(gl_GenVertexArrays, "glGenVertexArrays")
        || !GL_LOAD(gl_BindVertexArray, "glBindVertexArray")
        || !GL_LOAD(gl_DrawArrays, "glDrawArrays")
        || !GL_LOAD(gl_Viewport, "glViewport")
        || !GL_LOAD(gl_ActiveTexture, "glActiveTexture")) {
        SDL_SetError("missing OpenGL function");
        return -1;
    }

    /* Brogue is drawn at no more than 1080p, and scaled up by OpenGL. */
    width = SDL_min(mode.w, 1920);
    height = SDL_min(mode.h, 1080);
    /* Bytes R, G, B, A in memory: uploaded as GL_RGBA without conversion. */
    screen = SDL_CreateRGBSurfaceWithFormat(0, width, height, 32,
                                            SDL_PIXELFORMAT_ABGR8888);
    if (!screen)
        return -1;
    SDL_FillRect(screen, NULL, SDL_MapRGB(screen->format, 0, 0, 0));
    hint_top = height - HINT_BAR_HEIGHT(height);
    game = SDL_CreateRGBSurfaceWithFormatFrom(screen->pixels, width, hint_top,
                                              32, screen->pitch,
                                              SDL_PIXELFORMAT_ABGR8888);
    if (!game)
        return -1;

    gl_GenTextures(1, &texture);
    gl_ActiveTexture(GL_TEXTURE0);
    gl_BindTexture(GL_TEXTURE_2D, texture);
    gl_TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    gl_TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    gl_TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    gl_TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    gl_PixelStorei(GL_UNPACK_ALIGNMENT, 4);
    gl_PixelStorei(GL_UNPACK_ROW_LENGTH, screen->pitch / 4);
    gl_TexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, screen->w, screen->h, 0,
                  GL_RGBA, GL_UNSIGNED_BYTE, screen->pixels);

    program = gl_CreateProgram();
    gl_AttachShader(program, compile_shader(GL_VERTEX_SHADER, vertex_source));
    gl_AttachShader(program,
                    compile_shader(GL_FRAGMENT_SHADER, fragment_source));
    gl_LinkProgram(program);
    gl_GetProgramiv(program, GL_LINK_STATUS, &linked);
    if (!linked) {
        SDL_SetError("shader program failed to link");
        return -1;
    }
    gl_UseProgram(program);
    gl_Uniform1i(gl_GetUniformLocation(program, "surface"), 0);
    gl_GenVertexArrays(1, &vertex_array);
    gl_BindVertexArray(vertex_array);
    gl_Viewport(0, 0, mode.w, mode.h);
    return 0;
}

static void upload_rect(const SDL_Rect *r)
{
    gl_PixelStorei(GL_UNPACK_SKIP_PIXELS, r->x);
    gl_PixelStorei(GL_UNPACK_SKIP_ROWS, r->y);
    gl_TexSubImage2D(GL_TEXTURE_2D, 0, r->x, r->y, r->w, r->h, GL_RGBA,
                     GL_UNSIGNED_BYTE, screen->pixels);
}

/* ---- Text and symbols for the hint bar ---------------------------------- */

/* The bar's letters are Brogue's own: the text glyphs of its tile sheet
 * (assets/tiles.png, 128x232 pixels each, white on black), averaged down to
 * the size of a cell.  Read before the game loads the sheet itself, then
 * dropped. */
#define SHEET_TILE_W 128
#define SHEET_TILE_H 232
#define SHEET_COLS 16
#define CELL_W 15
#define CELL_H 27
#define FIRST_GLYPH 32
#define LAST_GLYPH 126

static unsigned char glyph_cover[LAST_GLYPH + 1][CELL_H][CELL_W];

static int load_glyphs(void)
{
    SDL_Surface *image, *sheet;
    int c, x, y;

    image = IMG_Load("/app0/assets/tiles.png");
    if (!image)
        return -1;
    sheet = SDL_ConvertSurfaceFormat(image, SDL_PIXELFORMAT_ARGB8888, 0);
    SDL_FreeSurface(image);
    if (!sheet)
        return -1;
    for (c = FIRST_GLYPH; c <= LAST_GLYPH; c++) {
        int column = c % SHEET_COLS, row = c / SHEET_COLS;

        for (y = 0; y < CELL_H; y++) {
            for (x = 0; x < CELL_W; x++) {
                /* the source pixels that fall in this cell */
                int x0 = x * SHEET_TILE_W / CELL_W;
                int x1 = (x + 1) * SHEET_TILE_W / CELL_W;
                int y0 = y * SHEET_TILE_H / CELL_H;
                int y1 = (y + 1) * SHEET_TILE_H / CELL_H;
                unsigned sum = 0, count = 0;
                int sx, sy;

                for (sy = y0; sy < y1; sy++) {
                    const Uint32 *line = (const Uint32 *) ((Uint8 *) sheet->pixels
                        + (row * SHEET_TILE_H + sy) * sheet->pitch);

                    for (sx = x0; sx < x1; sx++, count++)
                        sum += (line[column * SHEET_TILE_W + sx] >> 8) & 0xff;
                }
                glyph_cover[c][y][x] = count ? (unsigned char) (sum / count) : 0;
            }
        }
    }
    SDL_FreeSurface(sheet);
    return 0;
}

typedef struct { unsigned char r, g, b; } rgb;

static const rgb bar_background = { 12, 14, 24 };
static const rgb bar_text = { 220, 220, 230 };
static const rgb bar_dim = { 150, 150, 165 };

static void blend_pixel(int x, int y, rgb colour, float alpha)
{
    Uint8 *p;
    float a;

    if (x < 0 || y < 0 || x >= screen->w || y >= screen->h || alpha <= 0)
        return;
    a = alpha > 1 ? 1 : alpha;
    p = (Uint8 *) screen->pixels + y * screen->pitch + x * 4;
    p[0] = (Uint8) (p[0] + (colour.r - p[0]) * a);
    p[1] = (Uint8) (p[1] + (colour.g - p[1]) * a);
    p[2] = (Uint8) (p[2] + (colour.b - p[2]) * a);
    p[3] = 255;
}

static int draw_glyph(int x, int y, unsigned char c, rgb colour)
{
    int dx, dy;

    if (c < FIRST_GLYPH || c > LAST_GLYPH)
        return x + CELL_W;
    for (dy = 0; dy < CELL_H; dy++)
        for (dx = 0; dx < CELL_W; dx++)
            blend_pixel(x + dx, y + dy, colour, glyph_cover[c][dy][dx] / 255.0f);
    return x + CELL_W;
}

static int text_width(const char *s)
{
    return (int) strlen(s) * CELL_W;
}

static int draw_text(int x, int y, const char *s, rgb colour)
{
    while (*s)
        x = draw_glyph(x, y, (unsigned char) *s++, colour);
    return x;
}

/* The DualSense's face buttons, drawn as outlines in their PlayStation
 * colours inside a square box of CELL_H pixels. */
enum symbol { S_CROSS, S_CIRCLE, S_SQUARE, S_TRIANGLE };

static float clampf(float v, float lo, float hi)
{
    return v < lo ? lo : v > hi ? hi : v;
}

static float segment_distance(float px, float py, float ax, float ay,
                              float bx, float by)
{
    float vx = bx - ax, vy = by - ay;
    float t = clampf(((px - ax) * vx + (py - ay) * vy) / (vx * vx + vy * vy),
                     0, 1);
    float dx = px - (ax + vx * t), dy = py - (ay + vy * t);

    return sqrtf(dx * dx + dy * dy);
}

static void draw_symbol(int x, int y, enum symbol symbol, rgb colour)
{
    const float size = CELL_H, centre = size / 2, stroke = 2.6f;
    const float r = size * 0.36f;
    int px, py, sx, sy;

    for (py = 0; py < CELL_H; py++) {
        for (px = 0; px < CELL_H; px++) {
            float cover = 0;

            /* 3x3 samples per pixel for smooth edges */
            for (sy = 0; sy < 3; sy++) {
                for (sx = 0; sx < 3; sx++) {
                    float fx = px + (sx + 0.5f) / 3 - centre;
                    float fy = py + (sy + 0.5f) / 3 - centre;
                    float d;

                    switch (symbol) {
                    case S_CIRCLE:
                        d = fabsf(sqrtf(fx * fx + fy * fy) - r);
                        break;
                    case S_SQUARE: {
                        float qx = fabsf(fx) - r * 0.92f;
                        float qy = fabsf(fy) - r * 0.92f;
                        float ox = qx > 0 ? qx : 0, oy = qy > 0 ? qy : 0;
                        float inside = qx > qy ? qx : qy;

                        d = fabsf(sqrtf(ox * ox + oy * oy)
                                  + (inside < 0 ? inside : 0));
                        break;
                    }
                    case S_TRIANGLE: {
                        /* an upward triangle: three segments */
                        float top = -r * 1.05f, base = r * 0.78f, half = r * 1.05f;
                        float d1 = segment_distance(fx, fy, 0, top, -half, base);
                        float d2 = segment_distance(fx, fy, 0, top, half, base);
                        float d3 = segment_distance(fx, fy, -half, base, half, base);

                        d = d1 < d2 ? d1 : d2;
                        d = d < d3 ? d : d3;
                        break;
                    }
                    default: { /* S_CROSS */
                        float d1 = segment_distance(fx, fy, -r, -r, r, r);
                        float d2 = segment_distance(fx, fy, -r, r, r, -r);

                        d = d1 < d2 ? d1 : d2;
                        break;
                    }
                    }
                    if (d < stroke / 2)
                        cover += 1;
                }
            }
            blend_pixel(x + px, y + py, colour, cover / 9.0f);
        }
    }
}

/* A button named in words (L1, D-pad, Options): a rounded outline round it. */
static int draw_badge(int x, int y, const char *label, rgb colour)
{
    int w = text_width(label) + 14, h = CELL_H + 2, i;

    for (i = 3; i < w - 3; i++) {
        blend_pixel(x + i, y - 1, colour, 0.9f);
        blend_pixel(x + i, y + h - 2, colour, 0.9f);
    }
    for (i = 3; i < h - 4; i++) {
        blend_pixel(x, y + i, colour, 0.9f);
        blend_pixel(x + 1, y + i, colour, 0.9f);
        blend_pixel(x + w - 1, y + i, colour, 0.9f);
        blend_pixel(x + w - 2, y + i, colour, 0.9f);
    }
    blend_pixel(x + 1, y, colour, 0.9f);
    blend_pixel(x + 2, y - 1, colour, 0.9f);
    blend_pixel(x + w - 2, y, colour, 0.9f);
    blend_pixel(x + w - 3, y - 1, colour, 0.9f);
    blend_pixel(x + 1, y + h - 3, colour, 0.9f);
    blend_pixel(x + 2, y + h - 2, colour, 0.9f);
    blend_pixel(x + w - 2, y + h - 3, colour, 0.9f);
    blend_pixel(x + w - 3, y + h - 2, colour, 0.9f);
    draw_text(x + 7, y + 1, label, colour);
    return x + w;
}

/* ---- The button hint bar ------------------------------------------------- */

struct hint {
    int symbol;             /* an enum symbol, or -1 for a named button */
    const char *button;     /* the named button */
    const char *action;
};

#define SYM(s) (s), NULL
#define NAMED(n) -1, (n)
#define END_HINTS { 0, NULL, NULL }

static const rgb symbol_colour[] = {
    [S_CROSS] = { 120, 160, 255 }, [S_CIRCLE] = { 255, 110, 110 },
    [S_SQUARE] = { 240, 130, 210 }, [S_TRIANGLE] = { 70, 220, 180 },
};
static const rgb named_colour = { 190, 190, 205 };

static const struct hint map_hints[] = {
    { SYM(S_CROSS), "Look / travel" }, { SYM(S_SQUARE), "Explore" },
    { SYM(S_TRIANGLE), "Pack" },       { NAMED("Options"), "Menu" },
    { NAMED("L1 R1"), "Stairs" },      { NAMED("L2"), "Search" },
    { NAMED("R2"), "Rest" }, END_HINTS
};
static const struct hint cursor_hints[] = {
    { NAMED("D-pad"), "Move cursor" }, { SYM(S_CROSS), "Go here" },
    { SYM(S_TRIANGLE), "Next thing" }, { SYM(S_CIRCLE), "Cancel" }, END_HINTS
};
static const struct hint aim_hints[] = {
    { NAMED("D-pad"), "Aim" },         { SYM(S_CROSS), "Confirm" },
    { SYM(S_TRIANGLE), "Next target" }, { SYM(S_CIRCLE), "Cancel" }, END_HINTS
};
static const struct hint button_hints[] = {
    { NAMED("D-pad"), "Choose" }, { SYM(S_CROSS), "Select" },
    { SYM(S_CIRCLE), "Back" }, END_HINTS
};
static const struct hint acknowledge_hints[] = {
    { SYM(S_CROSS), "Continue" }, END_HINTS
};
static const struct hint text_hints[] = {
    { SYM(S_CIRCLE), "Cancel" }, END_HINTS
};
static const struct hint *const hint_sets[CONTEXT_COUNT] = {
    [CONTEXT_MAP] = map_hints,
    [CONTEXT_CURSOR] = cursor_hints,
    [CONTEXT_AIM] = aim_hints,
    [CONTEXT_BUTTONS] = button_hints,
    [CONTEXT_ACKNOWLEDGE] = acknowledge_hints,
    [CONTEXT_TEXT] = text_hints,
};

static int hint_context = -1;

static int hint_width(const struct hint *h)
{
    int w = h->symbol >= 0 ? CELL_H : text_width(h->button) + 14;

    return w + 12 + text_width(h->action);
}

static void fill_rect(int x, int y, int w, int h, rgb colour)
{
    SDL_Rect r = { x, y, w, h };

    SDL_FillRect(screen, &r, SDL_MapRGB(screen->format, colour.r, colour.g,
                                        colour.b));
}

/* The bar for what is being asked, centred, with a rule above it. */
static void draw_hints(int context)
{
    const struct hint *set = hint_sets[context >= 0 && context < CONTEXT_COUNT
                                           ? context : 0];
    const struct hint *h;
    const rgb rule = { 52, 58, 88 };
    int bar_height = screen->h - hint_top;
    int gap = 44, count = 0, total = 0;
    int x, y = hint_top + (bar_height - CELL_H) / 2 + 1;

    fill_rect(0, hint_top, screen->w, bar_height, bar_background);
    fill_rect(0, hint_top, screen->w, 2, rule);
    for (h = set; h->action; h++, count++)
        total += hint_width(h);
    if (count > 1 && total + gap * (count - 1) > screen->w - 64)
        gap = SDL_max(14, (screen->w - 64 - total) / (count - 1));
    total += gap * (count - 1);
    x = (screen->w - total) / 2;
    for (h = set; h->action; h++) {
        if (h != set)
            x += gap;
        if (h->symbol >= 0) {
            draw_symbol(x, y, (enum symbol) h->symbol, symbol_colour[h->symbol]);
            x += CELL_H;
        } else {
            x = draw_badge(x, y, h->button, named_colour);
        }
        x = draw_text(x + 12, y, h->action, bar_text);
    }
}

static void present(void)
{
    gl_DrawArrays(GL_TRIANGLES, 0, 3);
    SDL_GL_SwapWindow(window);
#ifdef PS5_SCREENSHOT
    /* Debug builds: the last frame shown, readable over FTP; written whole
     * and renamed, so a reader never sees half a file.  At most every three
     * seconds, as a save takes seconds. */
    {
        static Uint32 saved;

        if (!saved || SDL_TICKS_PASSED(SDL_GetTicks(), saved + 3000)) {
            if (SDL_SaveBMP(screen, "/data/brogue-screen.tmp") == 0)
                rename("/data/brogue-screen.tmp", "/data/brogue-screen.bmp");
            saved = SDL_GetTicks();
        }
    }
#endif
}

/* Redraw the hint bar if what is being asked has changed; true if it did. */
static int refresh_hints(void)
{
    int context = ps5HintContext();
    SDL_Rect bar;

    if (context == hint_context)
        return 0;
    hint_context = context;
    draw_hints(context);
    bar.x = 0, bar.y = hint_top, bar.w = screen->w, bar.h = screen->h - hint_top;
    upload_rect(&bar);
    return 1;
}

/* ---- Brogue's SDL calls, bound here with --wrap --------------------------- */

/* tiles.c asks for a window of its own: the OpenGL one is its only one. */
SDL_Window *__wrap_SDL_CreateWindow(const char *title, int x, int y, int w,
                                    int h, Uint32 flags)
{
    (void) title, (void) x, (void) y, (void) w, (void) h, (void) flags;
    return window;
}

/* ...and a renderer for it: a software one that draws into `game`. */
SDL_Renderer *__wrap_SDL_CreateRenderer(SDL_Window *w, int index, Uint32 flags)
{
    (void) w, (void) index, (void) flags;
    if (!renderer)
        renderer = SDL_CreateSoftwareRenderer(game);
    return renderer;
}

SDL_Renderer *__wrap_SDL_GetRenderer(SDL_Window *w)
{
    (void) w;
    return renderer;
}

/* The window is fixed, full screen and without an icon. */
int __wrap_SDL_SetWindowFullscreen(SDL_Window *w, Uint32 flags)
{
    (void) w, (void) flags;
    return 0;
}

Uint32 __real_SDL_GetWindowFlags(SDL_Window *w);
Uint32 __wrap_SDL_GetWindowFlags(SDL_Window *w)
{
    return __real_SDL_GetWindowFlags(w) | SDL_WINDOW_FULLSCREEN_DESKTOP;
}

void __wrap_SDL_SetWindowSize(SDL_Window *w, int width, int height)
{
    (void) w, (void) width, (void) height;
}

void __wrap_SDL_RestoreWindow(SDL_Window *w)
{
    (void) w;
}

void __wrap_SDL_SetWindowIcon(SDL_Window *w, SDL_Surface *icon)
{
    (void) w, (void) icon;
}

/* Anything Brogue draws marks the frame to be shown. */
int __real_SDL_RenderClear(SDL_Renderer *r);
int __wrap_SDL_RenderClear(SDL_Renderer *r)
{
    game_dirty = 1;
    return __real_SDL_RenderClear(r);
}

int __real_SDL_RenderFillRect(SDL_Renderer *r, const SDL_Rect *rect);
int __wrap_SDL_RenderFillRect(SDL_Renderer *r, const SDL_Rect *rect)
{
    game_dirty = 1;
    return __real_SDL_RenderFillRect(r, rect);
}

int __real_SDL_RenderCopy(SDL_Renderer *r, SDL_Texture *t,
                          const SDL_Rect *src, const SDL_Rect *dst);
int __wrap_SDL_RenderCopy(SDL_Renderer *r, SDL_Texture *t,
                          const SDL_Rect *src, const SDL_Rect *dst)
{
    game_dirty = 1;
    return __real_SDL_RenderCopy(r, t, src, dst);
}

/* The frame is finished: show it, with the hints under it. */
void __wrap_SDL_RenderPresent(SDL_Renderer *r)
{
    int hints_changed;

    (void) r;
    hints_changed = refresh_hints();
    if (!game_dirty && !hints_changed)
        return;
    if (game_dirty) {
        SDL_Rect all = { 0, 0, game->w, game->h };

        upload_rect(&all);
        game_dirty = 0;
    }
    present();
}

/* ---- Typing: the system's on-screen keyboard (IME dialog) ---------------- */

/* As ps5-payload-dev's SDL port declares them (src/video/ps5/
 * SDL_ps5keyboard.c).  The text is UTF-16: the PS5's wchar_t is 16 bits. */
typedef struct {
    int userId;
    int type;                   /* 1: basic Latin */
    uint64_t supportedLanguages;
    int enterLabel;             /* 0: default */
    int inputMethod;            /* 0: default */
    void *filter;
    uint32_t option;
    uint32_t maxTextLength;
    uint16_t *inputTextBuffer;
    float posx, posy;
    int halign, valign;         /* 1, 1: centred */
    const uint16_t *placeholder;
    const uint16_t *title;
    int8_t reserved[16];
} ime_dialog_param;

typedef struct {
    int outcome;                /* 0 OK, 1 cancelled, 2 aborted */
    int8_t reserved[12];
} ime_dialog_result;

enum { IME_STATUS_NONE, IME_STATUS_RUNNING, IME_STATUS_FINISHED };

/* A title is not given libSceImeDialog at start, and sceKernelDlsym finds
 * none of its functions once it is loaded (0x80020003).  Instead they are
 * linked as ordinary imports, through the SDK's libSceImeDialog.so stub, and
 * the module is loaded before the first call: the console binds an import
 * at its first call, from the modules loaded by then.  Until then its
 * address reads 0, so it is called directly, never through a pointer taken
 * early (as NetHack's ps5main.c does). */
int sceKernelLoadStartModule(const char *path, size_t argc, const void *argv,
                             uint32_t flags, void *option, int *result);
/* A title's sandbox shows the system libraries under /<random word>/ */
const char *sceKernelGetFsSandboxRandomWord(void);

int sceImeDialogInit(const ime_dialog_param *param, void *extended);
int sceImeDialogGetStatus(void);
int sceImeDialogGetResult(ime_dialog_result *result);
int sceImeDialogTerm(void);

static int ime_load(void)
{
    static int state; /* 0 not tried, 1 loaded, -1 unavailable */
    const char *word = sceKernelGetFsSandboxRandomWord();
    char path[128];
    int handle;

    if (state)
        return state > 0;
    state = -1;
    snprintf(path, sizeof path, "/%s/common/lib/libSceImeDialog.sprx",
             word ? word : "system");
    handle = sceKernelLoadStartModule(path, 0, NULL, 0, NULL, NULL);
    if (handle < 0) {
        printf("brogue: cannot load the on-screen keyboard from %s "
               "(0x%08x)\n", path, (unsigned) handle);
        return 0;
    }
    printf("brogue: on-screen keyboard module loaded (0x%08x)\n",
           (unsigned) handle);
    state = 1;
    return 1;
}

/* Ask for a line of text with the on-screen keyboard, the prompt as its
 * title, for getInputTextString() (IO.c).  Returns 1 with the text in
 * answer, 0 if cancelled, -1 if the keyboard cannot open. */
int ps5TextInput(const char *prompt, const char *initial, char *answer,
                 int size)
{
    static uint16_t title[128], text[256];
    ime_dialog_param param;
    ime_dialog_result result;
    int user = -1, status, rc, i;

    if (size < 2 || !screen || !ime_load())
        return -1;
    for (i = 0; prompt[i] && i < (int) SDL_arraysize(title) - 1; i++)
        title[i] = (unsigned char) prompt[i];
    title[i] = 0;
    memset(text, 0, sizeof text);
    for (i = 0; initial && initial[i] && i < size - 1
                && i < (int) SDL_arraysize(text) - 1; i++)
        text[i] = (unsigned char) initial[i];

    memset(&param, 0, sizeof param);
    sceUserServiceGetForegroundUser(&user);
    param.userId = user;
    param.type = 1; /* basic Latin: Brogue's text is ASCII */
    param.maxTextLength = (uint32_t) SDL_min(size - 1,
                                             (int) SDL_arraysize(text) - 1);
    param.inputTextBuffer = text;
    param.posx = screen->w / 2.0f;
    param.posy = screen->h / 2.0f;
    param.halign = param.valign = 1;
    param.title = title;

    rc = sceImeDialogInit(&param, NULL);
    if (rc < 0) {
        printf("brogue: on-screen keyboard unavailable (0x%08x)\n",
               (unsigned) rc);
        return -1;
    }
    printf("brogue: on-screen keyboard open: %s\n", prompt);
    /* the system draws the keyboard over the title; keep showing the game
     * underneath until it closes */
    while ((status = sceImeDialogGetStatus()) == IME_STATUS_RUNNING) {
        present();
        sceKernelUsleep(16000);
    }
    memset(&result, 0, sizeof result);
    rc = status == IME_STATUS_FINISHED ? sceImeDialogGetResult(&result) : -1;
    sceImeDialogTerm();
    printf("brogue: on-screen keyboard closed (status %d, result 0x%08x, "
           "outcome %d)\n", status, (unsigned) rc, result.outcome);
    if (rc < 0 || result.outcome != 0)
        return 0;
    for (i = 0; text[i] && i < size - 1; i++)
        answer[i] = text[i] < 128 ? (char) text[i] : '?';
    answer[i] = '\0';
    return 1;
}

/* ---- The controller ------------------------------------------------------- */

/* DualSense button indices from SDL's PS5 joystick driver. */
enum {
    B_CROSS = 0, B_CIRCLE, B_SQUARE, B_TRIANGLE, B_TOUCHPAD, B_SHARE,
    B_OPTIONS, B_L3, B_R3, B_L1, B_R1, B_UP, B_DOWN, B_LEFT, B_RIGHT,
    B_L2, B_R2, B_COUNT
};

#define STICK_DEADZONE 16000
#define REPEAT_DELAY_MS 350
#define REPEAT_RATE_MS 110

static SDL_mutex *rep_lock;
static SDL_Keycode rep_key;     /* key currently auto-repeating, or 0 */
static Uint32 rep_next;         /* SDL_GetTicks() of next repeat */
static int stick_x, stick_y;
static SDL_Keycode stick_key;

/* Printable characters go to Brogue as text, the rest as key presses. */
static void push_key(SDL_Keycode sym)
{
    SDL_Event e;

    memset(&e, 0, sizeof e);
    if (sym >= ' ' && sym < 127) {
        e.type = SDL_TEXTINPUT;
        e.text.text[0] = (char) sym;
        SDL_PushEvent(&e);
    } else {
        e.type = SDL_KEYDOWN;
        e.key.state = SDL_PRESSED;
        e.key.keysym.sym = sym;
        e.key.keysym.scancode = SDL_GetScancodeFromKey(sym);
        SDL_PushEvent(&e);
        e.type = SDL_KEYUP;
        e.key.state = SDL_RELEASED;
        SDL_PushEvent(&e);
    }
}

/* The key a button sends, for what Brogue is asking: 0 for none.  Brogue's
 * own screens give each key its meaning, so the pad sends the keys the
 * keyboard would: Return, Escape, the arrows, and the commands' letters. */
static SDL_Keycode button_key(int button)
{
    int context = ps5HintContext();
    int dungeon = context == CONTEXT_MAP;
    int pointing = context == CONTEXT_CURSOR || context == CONTEXT_AIM;

    switch (button) {
    case B_CROSS:    return SDLK_RETURN;     /* look and travel, choose, go on */
    case B_CIRCLE:   return SDLK_ESCAPE;     /* cancel, back */
    case B_SQUARE:   return dungeon ? 'x' : 0;          /* explore */
    case B_TRIANGLE: return dungeon ? 'i'               /* the pack */
                          : pointing ? SDLK_TAB : 0;    /* the next thing */
    case B_OPTIONS:  return dungeon ? (SDL_Keycode) PAD_MENU_KEY : 0;
    case B_SHARE:    return dungeon ? '?' : 0;          /* help */
    case B_TOUCHPAD: return dungeon ? 'M' : 0;          /* messages */
    case B_L3:       return dungeon ? 'Z' : 0;          /* rest until better */
    case B_R3:       return dungeon ? 'T' : 0;          /* throw again */
    case B_L1:       return dungeon || pointing ? '<' : 0;
    case B_R1:       return dungeon || pointing ? '>' : 0;
    case B_L2:       return dungeon ? 's' : 0;          /* search */
    case B_R2:       return dungeon ? 'z' : 0;          /* rest */
    case B_UP:       return SDLK_UP;
    case B_DOWN:     return SDLK_DOWN;
    case B_LEFT:     return SDLK_LEFT;
    case B_RIGHT:    return SDLK_RIGHT;
    default:         return 0;
    }
}

/* Held, these repeat: walking, moving the cursor or the focus, and
 * searching and resting. */
static int repeats(SDL_Keycode k)
{
    switch (k) {
    case SDLK_UP: case SDLK_DOWN: case SDLK_LEFT: case SDLK_RIGHT:
    case 'y': case 'u': case 'b': case 'n': case 's': case 'z':
        return 1;
    default:
        return 0;
    }
}

/* The left stick: the arrows and, where Brogue takes letters for them, the
 * diagonals.  Menus have the letters as hotkeys, so there it is the arrows
 * only, along the longer way. */
static SDL_Keycode stick_direction(int x, int y)
{
    int context = ps5HintContext();
    int dx = (x > STICK_DEADZONE) - (x < -STICK_DEADZONE);
    int dy = (y > STICK_DEADZONE) - (y < -STICK_DEADZONE);

    if (!dx && !dy)
        return 0;
    if (context == CONTEXT_MAP || context == CONTEXT_CURSOR
        || context == CONTEXT_AIM) {
        static const SDL_Keycode keys[3][3] = {
            /* dy=-1       dy=0         dy=1 */
            { 'y', SDLK_LEFT, 'b' },    /* dx=-1 */
            { SDLK_UP, 0, SDLK_DOWN },  /* dx=0 */
            { 'u', SDLK_RIGHT, 'n' },   /* dx=1 */
        };

        return keys[dx + 1][dy + 1];
    }
    if (abs(x) >= abs(y))
        return dx > 0 ? SDLK_RIGHT : SDLK_LEFT;
    return dy > 0 ? SDLK_DOWN : SDLK_UP;
}

/* Stop any repeat, as when the controller goes away with a button held:
 * otherwise the hero would walk on forever. */
static void cancel_repeat(void)
{
    SDL_LockMutex(rep_lock);
    rep_key = 0;
    SDL_UnlockMutex(rep_lock);
    stick_x = stick_y = 0;
    stick_key = 0;
}

static void start_repeat(SDL_Keycode k)
{
    SDL_LockMutex(rep_lock);
    rep_key = repeats(k) ? k : 0;
    rep_next = SDL_GetTicks() + REPEAT_DELAY_MS;
    SDL_UnlockMutex(rep_lock);
}

static void stop_repeat(SDL_Keycode k)
{
    SDL_LockMutex(rep_lock);
    if (rep_key == k)
        rep_key = 0;
    SDL_UnlockMutex(rep_lock);
}

/* Re-sends a held key so you can walk by holding; 1 if it did. */
static int repeat_due(void)
{
    int sent = 0;

    SDL_LockMutex(rep_lock);
    if (rep_key && SDL_TICKS_PASSED(SDL_GetTicks(), rep_next)) {
        push_key(rep_key);
        rep_next = SDL_GetTicks() + REPEAT_RATE_MS;
        sent = 1;
    }
    SDL_UnlockMutex(rep_lock);
    return sent;
}

/* Repeating is done from Brogue's own polling (SDL_PollEvent is wrapped at
 * link time): only after the controller's pending events have been read, so
 * a released button is never taken for a held one.  A timer thread would
 * repeat whenever a slow frame kept the release unread, and one press moved
 * twice.  An idle poll also keeps the hints in step with what is asked. */
int __real_SDL_PollEvent(SDL_Event *e);
int __wrap_SDL_PollEvent(SDL_Event *e)
{
    if (__real_SDL_PollEvent(e))
        return 1;
    if (repeat_due())
        return __real_SDL_PollEvent(e);
    if (refresh_hints())
        present();
    return 0;
}

static int SDLCALL input_filter(void *unused, SDL_Event *e)
{
    (void) unused;
    switch (e->type) {
    case SDL_JOYDEVICEADDED:
        SDL_JoystickOpen(e->jdevice.which);
        return 0;
    case SDL_JOYBUTTONDOWN:
        if (e->jbutton.button < B_COUNT) {
            SDL_Keycode k = button_key(e->jbutton.button);

            if (k) {
                push_key(k);
                start_repeat(k);
            }
        }
        return 0;
    case SDL_JOYBUTTONUP:
        if (e->jbutton.button < B_COUNT) {
            /* the key it sent may differ now: stop whatever repeats */
            SDL_LockMutex(rep_lock);
            rep_key = 0;
            SDL_UnlockMutex(rep_lock);
        }
        return 0;
    case SDL_JOYAXISMOTION: {
        SDL_Keycode k;

        if (e->jaxis.axis == 0)
            stick_x = e->jaxis.value;
        else if (e->jaxis.axis == 1)
            stick_y = e->jaxis.value;
        else
            return 0;
        k = stick_direction(stick_x, stick_y);
        if (k != stick_key) {
            if (stick_key)
                stop_repeat(stick_key);
            stick_key = k;
            if (k) {
                push_key(k);
                start_repeat(k);
            }
        }
        return 0;
    }
    case SDL_JOYDEVICEREMOVED:
        /* switched off or out of charge: let go of what it was holding.
         * (Not closed here: the filter can run while SDL is still walking
         * its list of joysticks.) */
        cancel_repeat();
        return 0;
    case SDL_JOYHATMOTION:
    case SDL_JOYBALLMOTION:
        return 0; /* d-pad arrives as buttons too; ignore the hat */
    default:
        return 1;
    }
}

#ifdef PS5_SCREENSHOT
/* Debug builds: type the contents of /data/brogue-input.txt, then remove
 * it, so the game can be driven over FTP.  "\n" is Return, and ^ escapes
 * the controller: ^[ Circle (Escape), ^X Cross, ^S Square, ^T Triangle,
 * ^O Options, ^C Create, ^P the touchpad, ^U ^D ^L ^R the d-pad, ^1 ^2 L1
 * and R1, ^3 ^4 L2 and R2, ^5 ^6 L3 and R3.  The pad's keys are those the
 * buttons would send for what Brogue is asking. */
static Uint32 remote_keys(Uint32 interval, void *unused)
{
    static const struct { char code; int button; } escapes[] = {
        { '[', B_CIRCLE }, { 'X', B_CROSS }, { 'S', B_SQUARE },
        { 'T', B_TRIANGLE }, { 'O', B_OPTIONS }, { 'C', B_SHARE },
        { 'P', B_TOUCHPAD }, { 'U', B_UP }, { 'D', B_DOWN },
        { 'L', B_LEFT }, { 'R', B_RIGHT }, { '1', B_L1 }, { '2', B_R1 },
        { '3', B_L2 }, { '4', B_R2 }, { '5', B_L3 }, { '6', B_R3 },
    };
    FILE *fp = fopen("/data/brogue-input.txt", "r");
    char keys[256];
    size_t n, i, e;

    (void) unused;
    if (!fp)
        return interval;
    n = fread(keys, 1, sizeof keys - 1, fp);
    fclose(fp);
    remove("/data/brogue-input.txt");
    for (i = 0; i < n; i++) {
        if (keys[i] == '^' && i + 1 < n) {
            for (e = 0; e < SDL_arraysize(escapes); e++)
                if (escapes[e].code == keys[i + 1]) {
                    SDL_Keycode k = button_key(escapes[e].button);

                    if (k)
                        push_key(k);
                }
            i++;
        } else if (keys[i] == '\n') {
            push_key(SDLK_RETURN);
        } else {
            push_key((unsigned char) keys[i]);
        }
    }
    return interval;
}
#endif

/* ---- Leaving --------------------------------------------------------------- */

extern void writeIntoConfig(void);
static int brogue_started; /* its settings are loaded: safe to write them back */

/* Brogue has saved or ended the game: say so, and how to leave. */
static void farewell(int status)
{
    const char *headline = status == 0 ? "Until next time."
                                       : "Brogue has stopped.";
    const char *advice = "Press the PS button and close Brogue to leave.";
    SDL_Rect all;
    int y;

    if (!screen)
        return;
    all.x = all.y = 0;
    all.w = screen->w;
    all.h = screen->h;
    fill_rect(0, 0, screen->w, screen->h, bar_background);
    y = screen->h / 2 - 48;
    draw_text((screen->w - text_width(headline)) / 2, y, headline, bar_text);
    draw_text((screen->w - text_width(advice)) / 2, y + 64, advice, bar_dim);
    upload_rect(&all);
    hint_context = -2; /* no hint bar over the message */
    present();
}

/* A native title may not exit: exit() raises SIGSYS.  Brogue has saved or
 * ended the game by now; save its settings (it does so with atexit, which
 * this does not reach), then wait for the player to close the title. */
void __wrap_exit(int status)
{
    printf("brogue: exited (status %d); close it from the home screen\n",
           status);
    if (brogue_started)
        writeIntoConfig();
    fflush(NULL);
    farewell(status);
    for (;;)
        sceKernelUsleep(1000000);
}

int main(int argc, char **argv)
{
    static char *brogue_argv[8];
    int brogue_argc = 0, i;

    /* A title has no console: Brogue's output goes to a file that can be
     * read back over FTP.  Unbuffered, so a crash loses nothing. */
    if (freopen("/data/brogue-ps5.log", "w", stdout)) {
        setvbuf(stdout, NULL, _IONBF, 0);
        dup2(fileno(stdout), fileno(stderr));
    }
    printf("Brogue starting\n");

    SDL_SetMainReady();
    mkdir(STORAGE, 0755);
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_JOYSTICK | SDL_INIT_TIMER) != 0
        || setup_presenter() != 0) {
        printf("brogue: display setup failed: %s\n", SDL_GetError());
        __wrap_exit(1);
    }
    if (load_glyphs() != 0)
        printf("brogue: no hint font: %s\n", IMG_GetError());
    rep_lock = SDL_CreateMutex();
    SDL_SetEventFilter(input_filter, NULL);
    for (i = 0; i < SDL_NumJoysticks(); i++)
        SDL_JoystickOpen(i);
#ifdef PS5_SCREENSHOT
    SDL_AddTimer(500, remote_keys, NULL);
#endif

    brogue_argv[brogue_argc++] = "/app0/eboot.bin";
    brogue_argv[brogue_argc++] = "--full-screen";
    for (i = 1; i < argc && brogue_argc < 7; i++)
        brogue_argv[brogue_argc++] = argv[i];
    brogue_argv[brogue_argc] = NULL;
    brogue_started = 1;
    brogue_main(brogue_argc, brogue_argv);
    __wrap_exit(0);
    return 0;
}
