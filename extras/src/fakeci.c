/*
 * fakeci: colour-index emulation for IRIX IGLOO (IRIS GL on OpenGL) under
 * QuickTransit.  Preload ahead of libGL.so and libglxsync with
 * FU_RLDN32_LIST (install-extras.sh preloads it into every n32 program; it
 * only changes anything in IRIS GL programs).
 *
 * The host X server only has TrueColor GLX visuals.  IGLOO wants a
 * colour-index (PseudoColor) visual for IRIS GL's default cmode windows, so:
 *  - XGetVisualInfo also returns PseudoColor "clones" of the TrueColor
 *    visuals, and glXGetConfig describes the clones as colour-index;
 *  - colormaps IGLOO creates on TrueColor visuals are kept here as a palette
 *    (the IRIS GL colour map), not on the X server;
 *  - in contexts created on a clone, index calls (glIndex*, glClearIndex,
 *    GL_COLOR_INDEX pixels, fog/material indexes, mode queries) are
 *    translated to RGB through that palette.
 * Changing a colour with mapcolor() doesn't recolour pixels already drawn.
 *
 * It also makes winopen() keep the program in the foreground.  IRIS GL
 * normally forks into the background there, after IGLOO has created GL
 * contexts, and a child forked after Mesa started its driver threads
 * deadlocks on Mesa's locks.
 *
 * Set FAKECI_DEBUG=1 to log what it does to stderr.
 */
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dlfcn.h>
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <GL/gl.h>
#include <GL/glx.h>

#define NCOLORS 4096		/* 12-bit colour index, like most IRIS GL hardware */
#define INDEX_BITS 12
#define MAXCMAPS 32
#define MAXCTX 64

static unsigned char pal[NCOLORS][3];
static int debug = -1;

static Colormap fake_cmaps[MAXCMAPS];
static int nfake_cmaps;
static GLXContext ci_ctx[MAXCTX];
static int nci_ctx;
static int cur_ci;		/* current context is an emulated colour-index one */
static Display *cur_dpy;	/* display and drawable of the last glXMakeCurrent */
static GLXDrawable cur_draw;

static GLfloat cur_index, clear_index;
static GLint index_shift, index_offset;

static void dbg(const char *fmt, ...)
{
	va_list ap;

	if (debug < 0)
		debug = getenv("FAKECI_DEBUG") != NULL;
	if (!debug)
		return;
	va_start(ap, fmt);
	fprintf(stderr, "fakeci: ");
	vfprintf(stderr, fmt, ap);
	va_end(ap);
}

static void *real_sym(const char *lib, const char *name)
{
	void *h = dlopen(lib, RTLD_LAZY);
	void *p = h ? dlsym(h, name) : NULL;

	if (!p) {
		fprintf(stderr, "fakeci: can't find %s in %s\n", name, lib);
		abort();
	}
	return p;
}

#define REAL(lib, ret, name, args) \
	static ret (*real_##name) args; \
	if (!real_##name) \
		real_##name = (ret (*) args)real_sym(lib, #name)

/* A symbol in the program's global scope: the program, its libraries and
 * the preloads. */
static void *global_sym(const char *name)
{
	static void *self;

	if (!self)
		self = dlopen(NULL, RTLD_LAZY);
	return self ? dlsym(self, name) : NULL;
}

/* fakeci is preloaded into every program.  Only IRIS GL programs (linked
 * with IGLOO's libgl.so, which has gconfig) get colour-index visuals and the
 * colormap changes; other programs would pick the fake PseudoColor visuals
 * or lose RetainPermanent resources. */
static int iris_gl(void)
{
	static int r = -1;

	if (r < 0) {
		r = global_sym("gconfig") != NULL;
		dbg("IRIS GL program: %s\n", r ? "yes" : "no");
	}
	return r;
}

/* libglxsync, later in the preload list, wraps some of the same calls.  IRIX
 * rld has no RTLD_NEXT, so it also exports them as glxsync_<name>. */
static void *next_sym(const char *lib, const char *name)
{
	char buf[64];
	void *p;

	snprintf(buf, sizeof buf, "glxsync_%s", name);
	p = global_sym(buf);
	return p ? p : real_sym(lib, name);
}
#define NEXT(lib, ret, name, args) \
	static ret (*real_##name) args; \
	if (!real_##name) \
		real_##name = (ret (*) args)next_sym(lib, #name)
#define GL "libGL.so"
#define X11 "libX11.so.1"

/* IRIS GL's default colour map: BLACK RED GREEN YELLOW BLUE MAGENTA CYAN WHITE */
static void init_palette(void)
{
	static int done;
	int i;

	if (done)
		return;
	done = 1;
	for (i = 0; i < 8; i++) {
		pal[i][0] = i & 1 ? 255 : 0;
		pal[i][1] = i & 2 ? 255 : 0;
		pal[i][2] = i & 4 ? 255 : 0;
	}
}

static const unsigned char *index_rgb(GLint i)
{
	init_palette();
	return pal[(unsigned)i & (NCOLORS - 1)];
}

/* ---- visuals ----
 *
 * Each TrueColor visual gets a PseudoColor clone with its own visual ID
 * (the real ID | FAKE_ID) and its own Visual struct, so IGLOO can tell
 * them apart when it looks a visual up by ID or by window.  Clones are
 * translated back to the real visual before anything reaches X or GLX.
 */

#define FAKE_ID 0x10000000	/* X resource IDs are 29 bits */
#define MAXVIS 512

static struct clone {
	VisualID id;		/* real ID */
	Visual *real;
	Visual fake;
	XVisualInfo real_info;	/* filled in by unclone() */
	int have_info;
} clones[MAXVIS];
static int nclones;

static struct clone *clone_by_id(VisualID id)
{
	int i;

	for (i = 0; i < nclones; i++)
		if (clones[i].id == id)
			return &clones[i];
	return NULL;
}

static struct clone *make_clone(Visual *real)
{
	struct clone *c = clone_by_id(real->visualid);

	if (c || nclones == MAXVIS)
		return c;
	c = &clones[nclones++];
	c->id = real->visualid;
	c->real = real;
	c->fake = *real;
	c->fake.visualid = real->visualid | FAKE_ID;
	c->fake.class = PseudoColor;
	c->fake.map_entries = NCOLORS;
	c->fake.red_mask = c->fake.green_mask = c->fake.blue_mask = 0;
	return c;
}

static int is_clone(const XVisualInfo *vi)
{
	return vi && (vi->visualid & FAKE_ID);
}

static Visual *real_visual(Visual *v)
{
	struct clone *c;

	if (!v || !(v->visualid & FAKE_ID))
		return v;
	c = clone_by_id(v->visualid & ~FAKE_ID);
	return c ? c->real : v;
}

/* The real description of a clone, kept in the clone so the pointer stays
 * valid after it's handed to QuickTransit. */
static XVisualInfo *unclone(XVisualInfo *vi)
{
	struct clone *c;

	if (!is_clone(vi) || !(c = clone_by_id(vi->visualid & ~FAKE_ID)))
		return vi;
	if (!c->have_info) {
		c->real_info = *vi;
		c->real_info.visual = c->real;
		c->real_info.visualid = c->id;
		c->real_info.class = c->real->class;
		c->real_info.colormap_size = c->real->map_entries;
		c->real_info.red_mask = c->real->red_mask;
		c->real_info.green_mask = c->real->green_mask;
		c->real_info.blue_mask = c->real->blue_mask;
		c->real_info.bits_per_rgb = c->real->bits_per_rgb;
		c->have_info = 1;
	}
	return &c->real_info;
}

static int tmpl_match(const XVisualInfo *v, long mask, const XVisualInfo *t)
{
	if ((mask & VisualIDMask) && v->visualid != t->visualid)
		return 0;
	if ((mask & VisualClassMask) && v->class != t->class)
		return 0;
	if ((mask & VisualColormapSizeMask) && v->colormap_size != t->colormap_size)
		return 0;
	if ((mask & VisualRedMaskMask) && v->red_mask != t->red_mask)
		return 0;
	if ((mask & VisualGreenMaskMask) && v->green_mask != t->green_mask)
		return 0;
	if ((mask & VisualBlueMaskMask) && v->blue_mask != t->blue_mask)
		return 0;
	return 1;
}

XVisualInfo *XGetVisualInfo(Display *dpy, long mask, XVisualInfo *tmpl, int *n)
{
	REAL(X11, XVisualInfo *, XGetVisualInfo, (Display *, long, XVisualInfo *, int *));
	/* fields a clone differs in are matched here, not by Xlib */
	long own = VisualIDMask | VisualClassMask | VisualColormapSizeMask |
	    VisualRedMaskMask | VisualGreenMaskMask | VisualBlueMaskMask;
	XVisualInfo t, *r, *out;
	int nr, i, k = 0;

	if (!iris_gl())
		return real_XGetVisualInfo(dpy, mask, tmpl, n);
	t = *tmpl;
	if (mask & VisualIDMask) {
		/* let Xlib match the real ID; tmpl_match checks fake vs real */
		t.visualid &= ~FAKE_ID;
		r = real_XGetVisualInfo(dpy, (mask & ~own) | VisualIDMask, &t, &nr);
	} else
		r = real_XGetVisualInfo(dpy, mask & ~own, &t, &nr);
	if (!r) {
		*n = 0;
		return NULL;
	}
	out = malloc(sizeof(*out) * nr * 2);
	for (i = 0; i < nr; i++) {
		XVisualInfo c = r[i];
		struct clone *cl;

		if (tmpl_match(&r[i], mask, tmpl))
			out[k++] = r[i];
		if (r[i].class != TrueColor || !(cl = make_clone(r[i].visual)))
			continue;
		c.visual = &cl->fake;
		c.visualid = cl->fake.visualid;
		c.class = PseudoColor;
		c.colormap_size = NCOLORS;
		c.red_mask = c.green_mask = c.blue_mask = 0;
		if (tmpl_match(&c, mask, tmpl))
			out[k++] = c;
	}
	XFree(r);
	if (!(mask & VisualIDMask))
		dbg("XGetVisualInfo(mask 0x%lx) -> %d (%d real)\n", mask, k, nr);
	if (!k) {
		free(out);
		out = NULL;
	}
	*n = k;
	return out;
}

int glXGetConfig(Display *dpy, XVisualInfo *vi, int attr, int *value)
{
	REAL(GL, int, glXGetConfig, (Display *, XVisualInfo *, int, int *));

	if (is_clone(vi)) {
		switch (attr) {
		case GLX_RGBA:
			*value = False;
			return 0;
		case GLX_BUFFER_SIZE:
			*value = INDEX_BITS;
			return 0;
		case GLX_RED_SIZE:
		case GLX_GREEN_SIZE:
		case GLX_BLUE_SIZE:
		case GLX_ALPHA_SIZE:
		case GLX_ACCUM_RED_SIZE:
		case GLX_ACCUM_GREEN_SIZE:
		case GLX_ACCUM_BLUE_SIZE:
		case GLX_ACCUM_ALPHA_SIZE:
			*value = 0;
			return 0;
		}
		vi = unclone(vi);
	}
	return real_glXGetConfig(dpy, vi, attr, value);
}

GLXContext glXCreateContext(Display *dpy, XVisualInfo *vi, GLXContext share, Bool direct)
{
	REAL(GL, GLXContext, glXCreateContext, (Display *, XVisualInfo *, GLXContext, Bool));
	GLXContext ctx;
	int ci = is_clone(vi);

	ctx = real_glXCreateContext(dpy, unclone(vi), share, direct);
	dbg("glXCreateContext(visual 0x%lx%s) -> %p\n", vi ? vi->visualid : 0,
	    ci ? " colour-index" : "", (void *)ctx);
	if (ctx && ci && nci_ctx < MAXCTX)
		ci_ctx[nci_ctx++] = ctx;
	return ctx;
}

static int ctx_is_ci(GLXContext ctx)
{
	int i;

	for (i = 0; i < nci_ctx; i++)
		if (ci_ctx[i] == ctx)
			return 1;
	return 0;
}

void glXDestroyContext(Display *dpy, GLXContext ctx)
{
	REAL(GL, void, glXDestroyContext, (Display *, GLXContext));
	int i;

	for (i = 0; i < nci_ctx; i++)
		if (ci_ctx[i] == ctx) {
			ci_ctx[i] = ci_ctx[--nci_ctx];
			break;
		}
	real_glXDestroyContext(dpy, ctx);
}

static void set_color(GLfloat index);

Bool glXMakeCurrent(Display *dpy, GLXDrawable d, GLXContext ctx)
{
	NEXT(GL, Bool, glXMakeCurrent, (Display *, GLXDrawable, GLXContext));
	Bool r = real_glXMakeCurrent(dpy, d, ctx);

	if (r) {
		cur_ci = ctx && ctx_is_ci(ctx);
		cur_dpy = dpy;
		cur_draw = d;
	}
	dbg("glXMakeCurrent(dpy %p, 0x%lx, %p) = %d%s\n", (void *)dpy, (unsigned long)d, (void *)ctx, r,
	    cur_ci ? " colour-index" : "");
	return r;
}

/* IGLOO swaps through a different Display connection from the one it made
 * the context current on.  Mesa then has no GLX drawable for it on that
 * connection and the swap does nothing, so use the current context's. */
void glXSwapBuffers(Display *dpy, GLXDrawable d)
{
	NEXT(GL, void, glXSwapBuffers, (Display *, GLXDrawable));
	static unsigned long n;

	if (!iris_gl()) {
		real_glXSwapBuffers(dpy, d);
		return;
	}
	if (++n <= 3 || n % 500 == 0)
		dbg("glXSwapBuffers(dpy %p, 0x%lx) #%lu%s\n", (void *)dpy, (unsigned long)d, n,
		    d == cur_draw && dpy != cur_dpy ? " (using current context's display)" : "");
	if (d == cur_draw && cur_dpy)
		dpy = cur_dpy;
	real_glXSwapBuffers(dpy, d);
}

/* ---- colormaps ---- */

static int is_fake_cmap(Colormap c)
{
	int i;

	for (i = 0; i < nfake_cmaps; i++)
		if (fake_cmaps[i] == c)
			return 1;
	return 0;
}

Colormap XCreateColormap(Display *dpy, Window w, Visual *v, int alloc)
{
	REAL(X11, Colormap, XCreateColormap, (Display *, Window, Visual *, int));
	Visual *rv = real_visual(v);
	Colormap c;

	if (rv == v && (v->class != TrueColor || alloc != AllocAll))
		return real_XCreateColormap(dpy, w, v, alloc);
	c = real_XCreateColormap(dpy, w, rv, AllocNone);
	if (nfake_cmaps < MAXCMAPS)
		fake_cmaps[nfake_cmaps++] = c;
	dbg("XCreateColormap(visual 0x%lx, alloc %d) -> 0x%lx, emulated\n",
	    v->visualid, alloc, c);
	return c;
}

int XFreeColormap(Display *dpy, Colormap c)
{
	REAL(X11, int, XFreeColormap, (Display *, Colormap));
	int i;

	for (i = 0; i < nfake_cmaps; i++)
		if (fake_cmaps[i] == c) {
			fake_cmaps[i] = fake_cmaps[--nfake_cmaps];
			break;
		}
	return real_XFreeColormap(dpy, c);
}

static void store(const XColor *c)
{
	unsigned i = c->pixel & (NCOLORS - 1);

	init_palette();
	if (c->flags & DoRed)
		pal[i][0] = c->red >> 8;
	if (c->flags & DoGreen)
		pal[i][1] = c->green >> 8;
	if (c->flags & DoBlue)
		pal[i][2] = c->blue >> 8;
}

int XStoreColor(Display *dpy, Colormap cm, XColor *c)
{
	REAL(X11, int, XStoreColor, (Display *, Colormap, XColor *));

	if (!is_fake_cmap(cm))
		return real_XStoreColor(dpy, cm, c);
	store(c);
	if (cur_ci)
		set_color(cur_index);
	return 1;
}

int XStoreColors(Display *dpy, Colormap cm, XColor *c, int n)
{
	REAL(X11, int, XStoreColors, (Display *, Colormap, XColor *, int));
	int i;

	if (!is_fake_cmap(cm))
		return real_XStoreColors(dpy, cm, c, n);
	for (i = 0; i < n; i++)
		store(&c[i]);
	dbg("XStoreColors(%d) from pixel %lu\n", n, n ? c[0].pixel : 0);
	if (cur_ci)
		set_color(cur_index);
	return 1;
}

int XQueryColor(Display *dpy, Colormap cm, XColor *c)
{
	REAL(X11, int, XQueryColor, (Display *, Colormap, XColor *));
	const unsigned char *rgb;

	if (!is_fake_cmap(cm))
		return real_XQueryColor(dpy, cm, c);
	rgb = index_rgb(c->pixel);
	c->red = rgb[0] * 257;
	c->green = rgb[1] * 257;
	c->blue = rgb[2] * 257;
	c->flags = DoRed | DoGreen | DoBlue;
	return 1;
}

Status XAllocColorCells(Display *dpy, Colormap cm, Bool contig, unsigned long *masks,
    unsigned nplanes, unsigned long *pixels, unsigned npixels)
{
	REAL(X11, Status, XAllocColorCells, (Display *, Colormap, Bool, unsigned long *,
	    unsigned, unsigned long *, unsigned));
	unsigned i;

	if (!is_fake_cmap(cm))
		return real_XAllocColorCells(dpy, cm, contig, masks, nplanes, pixels, npixels);
	dbg("XAllocColorCells(%u planes, %u pixels)\n", nplanes, npixels);
	for (i = 0; i < nplanes; i++)
		masks[i] = 1UL << i;
	for (i = 0; i < npixels; i++)
		pixels[i] = i << nplanes;
	return 1;
}

#define MAXWIN 256
static struct {
	Window w;
	Visual *fake;
} ci_wins[MAXWIN];
static int nci_wins;

Window XCreateWindow(Display *dpy, Window parent, int x, int y, unsigned w, unsigned h,
    unsigned border, int depth, unsigned cls, Visual *v, unsigned long mask,
    XSetWindowAttributes *a)
{
	REAL(X11, Window, XCreateWindow, (Display *, Window, int, int, unsigned, unsigned,
	    unsigned, int, unsigned, Visual *, unsigned long, XSetWindowAttributes *));
	Visual *rv = real_visual(v);
	Window win = real_XCreateWindow(dpy, parent, x, y, w, h, border, depth, cls, rv, mask, a);

	if (win && rv != v && nci_wins < MAXWIN) {
		ci_wins[nci_wins].w = win;
		ci_wins[nci_wins++].fake = v;
	}
	dbg("XCreateWindow(%ux%u depth %d visual 0x%lx mask 0x%lx colormap 0x%lx) -> 0x%lx\n",
	    w, h, depth, v ? v->visualid : 0, mask, (mask & CWColormap) ? a->colormap : 0, win);
	return win;
}

int XDestroyWindow(Display *dpy, Window w)
{
	REAL(X11, int, XDestroyWindow, (Display *, Window));
	int i;

	for (i = 0; i < nci_wins; i++)
		if (ci_wins[i].w == w) {
			ci_wins[i] = ci_wins[--nci_wins];
			break;
		}
	return real_XDestroyWindow(dpy, w);
}

Status XGetWindowAttributes(Display *dpy, Window w, XWindowAttributes *a)
{
	REAL(X11, Status, XGetWindowAttributes, (Display *, Window, XWindowAttributes *));
	Status r = real_XGetWindowAttributes(dpy, w, a);
	int i;

	for (i = 0; r && i < nci_wins; i++)
		if (ci_wins[i].w == w) {
			a->visual = ci_wins[i].fake;
			break;
		}
	return r;
}

/*
 * IGLOO shares colormaps between programs: a helper connection set to
 * RetainPermanent creates one per GL visual and publishes them in root
 * window properties.  The palette for an emulated colormap lives in this
 * process, so another process's colormaps can't be used.  Hide the
 * properties and don't publish.  The helper connection is kept open instead
 * of being retained, so its colormaps last until the program exits.
 * Xmu's own retained standard colormaps (RGB_DEFAULT_MAP) are left alone.
 */
static int shared_cmap_prop(Display *dpy, Atom a)
{
	static Display *d;
	static Atom gl, igloo;

	if (!iris_gl())
		return 0;
	if (dpy != d) {
		d = dpy;
		gl = XInternAtom(dpy, "_SGI_GL_DEFAULT_COLORMAPS", True);
		igloo = XInternAtom(dpy, "_SGI_IGLOO_DEFAULT_COLORMAPS", True);
	}
	return a != None && (a == gl || a == igloo);
}

int XGetWindowProperty(Display *dpy, Window w, Atom prop, long off, long len, Bool del,
    Atom req, Atom *type, int *format, unsigned long *n, unsigned long *after,
    unsigned char **data)
{
	REAL(X11, int, XGetWindowProperty, (Display *, Window, Atom, long, long, Bool, Atom,
	    Atom *, int *, unsigned long *, unsigned long *, unsigned char **));

	if (shared_cmap_prop(dpy, prop)) {
		*type = None;
		*format = 0;
		*n = *after = 0;
		*data = NULL;
		return Success;
	}
	return real_XGetWindowProperty(dpy, w, prop, off, len, del, req, type, format, n,
	    after, data);
}

int XChangeProperty(Display *dpy, Window w, Atom prop, Atom type, int format, int mode,
    const unsigned char *data, int n)
{
	NEXT(X11, int, XChangeProperty, (Display *, Window, Atom, Atom, int, int,
	    const unsigned char *, int));

	if (shared_cmap_prop(dpy, prop)) {
		dbg("not publishing shared colormaps\n");
		return 1;
	}
	return real_XChangeProperty(dpy, w, prop, type, format, mode, data, n);
}

#define MAXKEEP 8
static Display *kept[MAXKEEP];
static int nkept, in_xmu;

int XSetCloseDownMode(Display *dpy, int mode)
{
	REAL(X11, int, XSetCloseDownMode, (Display *, int));

	if (!iris_gl() || in_xmu || mode == DestroyAll || nkept == MAXKEEP)
		return real_XSetCloseDownMode(dpy, mode);
	dbg("XSetCloseDownMode(%d): keeping the connection open instead\n", mode);
	kept[nkept++] = dpy;
	return 1;
}

int XCloseDisplay(Display *dpy)
{
	REAL(X11, int, XCloseDisplay, (Display *));
	int i;

	for (i = 0; i < nkept; i++)
		if (kept[i] == dpy)
			return 0;
	return real_XCloseDisplay(dpy);
}

Status XGetRGBColormaps(Display *dpy, Window w, XStandardColormap **maps, int *n, Atom prop)
{
	REAL(X11, Status, XGetRGBColormaps, (Display *, Window, XStandardColormap **, int *, Atom));
	Status r = real_XGetRGBColormaps(dpy, w, maps, n, prop);

	dbg("XGetRGBColormaps(0x%lx, atom %lu) = %d n %d\n", w, prop, r, r ? *n : 0);
	return r;
}

Status XmuLookupStandardColormap(Display *dpy, int screen, VisualID vid, unsigned depth,
    Atom prop, Bool replace, Bool retain)
{
	REAL("libXmu.so", Status, XmuLookupStandardColormap, (Display *, int, VisualID, unsigned,
	    Atom, Bool, Bool));
	Status r;

	in_xmu = 1;
	r = real_XmuLookupStandardColormap(dpy, screen, vid, depth, prop, replace, retain);
	in_xmu = 0;

	dbg("XmuLookupStandardColormap(visual 0x%lx, depth %u, atom %lu) = %d\n",
	    vid, depth, prop, r);
	return r;
}

/* ---- IRIS GL ---- */

long winopen(const char *title)
{
	REAL("libgl.so", long, winopen, (const char *));
	REAL("libgl.so", void, foreground, (void));

	real_foreground();
	return real_winopen(title);
}

/* ---- GL ---- */

static void set_color(GLfloat index)
{
	REAL(GL, void, glColor4ub, (GLubyte, GLubyte, GLubyte, GLubyte));
	const unsigned char *rgb = index_rgb((GLint)index);

	real_glColor4ub(rgb[0], rgb[1], rgb[2], 255);
}

static void rgbaf(GLint index, GLfloat *out)
{
	const unsigned char *rgb = index_rgb(index);

	out[0] = rgb[0] / 255.0f;
	out[1] = rgb[1] / 255.0f;
	out[2] = rgb[2] / 255.0f;
	out[3] = 1.0f;
}

/* Index calls do nothing in an RGBA context, so they aren't passed on
 * (QuickTransit's GL bridge doesn't implement them). */
#define INDEX_FN(name, type) \
void name(type c) \
{ \
	cur_index = c; \
	if (cur_ci) \
		set_color(c); \
}
INDEX_FN(glIndexi, GLint)
INDEX_FN(glIndexf, GLfloat)
INDEX_FN(glIndexs, GLshort)
INDEX_FN(glIndexd, GLdouble)
INDEX_FN(glIndexub, GLubyte)

#define INDEXV_FN(name, type) \
void name(const type *c) \
{ \
	cur_index = *c; \
	if (cur_ci) \
		set_color(*c); \
}
INDEXV_FN(glIndexiv, GLint)
INDEXV_FN(glIndexfv, GLfloat)
INDEXV_FN(glIndexsv, GLshort)
INDEXV_FN(glIndexdv, GLdouble)
INDEXV_FN(glIndexubv, GLubyte)

void glClearIndex(GLfloat c)
{
	REAL(GL, void, glClearColor, (GLclampf, GLclampf, GLclampf, GLclampf));
	GLfloat f[4];

	clear_index = c;
	if (!cur_ci)
		return;
	rgbaf((GLint)c, f);
	real_glClearColor(f[0], f[1], f[2], f[3]);
}

void glIndexMask(GLuint m)
{
	REAL(GL, void, glColorMask, (GLboolean, GLboolean, GLboolean, GLboolean));

	if (cur_ci) {
		GLboolean on = m != 0;

		real_glColorMask(on, on, on, on);
	}
}

void glFogi(GLenum p, GLint v)
{
	REAL(GL, void, glFogi, (GLenum, GLint));
	REAL(GL, void, glFogfv, (GLenum, const GLfloat *));
	GLfloat f[4];

	if (cur_ci && p == GL_FOG_INDEX) {
		rgbaf(v, f);
		real_glFogfv(GL_FOG_COLOR, f);
	} else
		real_glFogi(p, v);
}

void glFogf(GLenum p, GLfloat v)
{
	REAL(GL, void, glFogf, (GLenum, GLfloat));
	REAL(GL, void, glFogfv, (GLenum, const GLfloat *));
	GLfloat f[4];

	if (cur_ci && p == GL_FOG_INDEX) {
		rgbaf((GLint)v, f);
		real_glFogfv(GL_FOG_COLOR, f);
	} else
		real_glFogf(p, v);
}

void glFogfv(GLenum p, const GLfloat *v)
{
	REAL(GL, void, glFogfv, (GLenum, const GLfloat *));
	GLfloat f[4];

	if (cur_ci && p == GL_FOG_INDEX) {
		rgbaf((GLint)*v, f);
		real_glFogfv(GL_FOG_COLOR, f);
	} else
		real_glFogfv(p, v);
}

/* Colour-index lighting has no RGB equivalent; use the three indexes'
 * colours as ambient, diffuse and specular. */
void glMaterialfv(GLenum face, GLenum p, const GLfloat *v)
{
	REAL(GL, void, glMaterialfv, (GLenum, GLenum, const GLfloat *));
	GLfloat f[4];

	if (!cur_ci || p != GL_COLOR_INDEXES) {
		real_glMaterialfv(face, p, v);
		return;
	}
	rgbaf((GLint)v[0], f);
	real_glMaterialfv(face, GL_AMBIENT, f);
	rgbaf((GLint)v[1], f);
	real_glMaterialfv(face, GL_DIFFUSE, f);
	rgbaf((GLint)v[2], f);
	real_glMaterialfv(face, GL_SPECULAR, f);
}

void glPixelTransferf(GLenum p, GLfloat v)
{
	REAL(GL, void, glPixelTransferf, (GLenum, GLfloat));

	if (p == GL_INDEX_SHIFT)
		index_shift = (GLint)v;
	else if (p == GL_INDEX_OFFSET)
		index_offset = (GLint)v;
	real_glPixelTransferf(p, v);
}

void glPixelTransferi(GLenum p, GLint v)
{
	REAL(GL, void, glPixelTransferi, (GLenum, GLint));

	if (p == GL_INDEX_SHIFT)
		index_shift = v;
	else if (p == GL_INDEX_OFFSET)
		index_offset = v;
	real_glPixelTransferi(p, v);
}

/* Answer the mode queries the way a colour-index context would.  Returns 1
 * if handled. */
static int ci_get(GLenum p, GLfloat *v)
{
	REAL(GL, void, glGetIntegerv, (GLenum, GLint *));
	GLint m;

	if (!cur_ci)
		return 0;
	switch (p) {
	case GL_INDEX_MODE:
		*v = 1;
		return 1;
	case GL_RGBA_MODE:
		*v = 0;
		return 1;
	case GL_INDEX_BITS:
		*v = INDEX_BITS;
		return 1;
	case GL_CURRENT_INDEX:
		*v = cur_index;
		return 1;
	case GL_INDEX_CLEAR_VALUE:
		*v = clear_index;
		return 1;
	case GL_INDEX_WRITEMASK:
		real_glGetIntegerv(GL_COLOR_WRITEMASK, &m);
		*v = m ? NCOLORS - 1 : 0;
		return 1;
	}
	return 0;
}

void glGetIntegerv(GLenum p, GLint *v)
{
	REAL(GL, void, glGetIntegerv, (GLenum, GLint *));
	GLfloat f;

	if (ci_get(p, &f))
		*v = (GLint)f;
	else
		real_glGetIntegerv(p, v);
}

void glGetFloatv(GLenum p, GLfloat *v)
{
	REAL(GL, void, glGetFloatv, (GLenum, GLfloat *));

	if (!ci_get(p, v))
		real_glGetFloatv(p, v);
}

void glGetBooleanv(GLenum p, GLboolean *v)
{
	REAL(GL, void, glGetBooleanv, (GLenum, GLboolean *));
	GLfloat f;

	if (ci_get(p, &f))
		*v = f != 0;
	else
		real_glGetBooleanv(p, v);
}

static GLint fetch_index(const void *base, GLenum type, size_t i)
{
	switch (type) {
	case GL_UNSIGNED_BYTE:
		return ((const GLubyte *)base)[i];
	case GL_BYTE:
		return ((const GLbyte *)base)[i];
	case GL_UNSIGNED_SHORT:
		return ((const GLushort *)base)[i];
	case GL_SHORT:
		return ((const GLshort *)base)[i];
	case GL_UNSIGNED_INT:
		return ((const GLuint *)base)[i];
	case GL_INT:
		return ((const GLint *)base)[i];
	case GL_FLOAT:
		return (GLint)((const GLfloat *)base)[i];
	}
	return 0;
}

static int type_size(GLenum type)
{
	switch (type) {
	case GL_UNSIGNED_BYTE:
	case GL_BYTE:
		return 1;
	case GL_UNSIGNED_SHORT:
	case GL_SHORT:
		return 2;
	case GL_UNSIGNED_INT:
	case GL_INT:
	case GL_FLOAT:
		return 4;
	}
	return 0;
}

static GLint shift_index(GLint i)
{
	i = index_shift >= 0 ? i << index_shift : i >> -index_shift;
	return i + index_offset;
}

void glDrawPixels(GLsizei w, GLsizei h, GLenum format, GLenum type, const GLvoid *data)
{
	REAL(GL, void, glDrawPixels, (GLsizei, GLsizei, GLenum, GLenum, const GLvoid *));
	REAL(GL, void, glGetIntegerv, (GLenum, GLint *));
	REAL(GL, void, glPixelStorei, (GLenum, GLint));
	GLint rowlen, align, skiprows, skippix;
	int es = type_size(type);
	size_t stride;
	unsigned char *buf, *o;
	int x, y;

	if (!cur_ci || format != GL_COLOR_INDEX || !es || w <= 0 || h <= 0) {
		real_glDrawPixels(w, h, format, type, data);
		return;
	}
	real_glGetIntegerv(GL_UNPACK_ROW_LENGTH, &rowlen);
	real_glGetIntegerv(GL_UNPACK_ALIGNMENT, &align);
	real_glGetIntegerv(GL_UNPACK_SKIP_ROWS, &skiprows);
	real_glGetIntegerv(GL_UNPACK_SKIP_PIXELS, &skippix);
	stride = (size_t)(rowlen > 0 ? rowlen : w) * es;
	stride = (stride + align - 1) / align * align;

	o = buf = malloc((size_t)w * h * 4);
	if (!buf)
		return;
	for (y = 0; y < h; y++) {
		const char *row = (const char *)data + (size_t)(y + skiprows) * stride +
		    (size_t)skippix * es;

		for (x = 0; x < w; x++) {
			const unsigned char *rgb = index_rgb(shift_index(fetch_index(row, type, x)));

			*o++ = rgb[0];
			*o++ = rgb[1];
			*o++ = rgb[2];
			*o++ = 255;
		}
	}
	real_glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
	real_glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
	real_glPixelStorei(GL_UNPACK_SKIP_ROWS, 0);
	real_glPixelStorei(GL_UNPACK_SKIP_PIXELS, 0);
	real_glDrawPixels(w, h, GL_RGBA, GL_UNSIGNED_BYTE, buf);
	real_glPixelStorei(GL_UNPACK_ROW_LENGTH, rowlen);
	real_glPixelStorei(GL_UNPACK_ALIGNMENT, align);
	real_glPixelStorei(GL_UNPACK_SKIP_ROWS, skiprows);
	real_glPixelStorei(GL_UNPACK_SKIP_PIXELS, skippix);
	free(buf);
}

/* Map an RGB value back to the palette index that produces it (first match). */
static GLint rgb_index(const unsigned char *rgb)
{
	static GLint last;
	int i;

	init_palette();
	if (!memcmp(pal[last], rgb, 3))
		return last;
	for (i = 0; i < NCOLORS; i++)
		if (!memcmp(pal[i], rgb, 3))
			return last = i;
	return 0;
}

void glReadPixels(GLint x, GLint y, GLsizei w, GLsizei h, GLenum format, GLenum type, GLvoid *data)
{
	REAL(GL, void, glReadPixels, (GLint, GLint, GLsizei, GLsizei, GLenum, GLenum, GLvoid *));
	REAL(GL, void, glGetIntegerv, (GLenum, GLint *));
	REAL(GL, void, glPixelStorei, (GLenum, GLint));
	GLint rowlen, align, skiprows, skippix;
	int es = type_size(type);
	size_t stride;
	unsigned char *buf, *in;
	int i, j;

	if (!cur_ci || format != GL_COLOR_INDEX || !es || w <= 0 || h <= 0) {
		real_glReadPixels(x, y, w, h, format, type, data);
		return;
	}
	real_glGetIntegerv(GL_PACK_ROW_LENGTH, &rowlen);
	real_glGetIntegerv(GL_PACK_ALIGNMENT, &align);
	real_glGetIntegerv(GL_PACK_SKIP_ROWS, &skiprows);
	real_glGetIntegerv(GL_PACK_SKIP_PIXELS, &skippix);
	stride = (size_t)(rowlen > 0 ? rowlen : w) * es;
	stride = (stride + align - 1) / align * align;

	in = buf = malloc((size_t)w * h * 4);
	if (!buf)
		return;
	real_glPixelStorei(GL_PACK_ROW_LENGTH, 0);
	real_glPixelStorei(GL_PACK_ALIGNMENT, 1);
	real_glPixelStorei(GL_PACK_SKIP_ROWS, 0);
	real_glPixelStorei(GL_PACK_SKIP_PIXELS, 0);
	real_glReadPixels(x, y, w, h, GL_RGBA, GL_UNSIGNED_BYTE, buf);
	real_glPixelStorei(GL_PACK_ROW_LENGTH, rowlen);
	real_glPixelStorei(GL_PACK_ALIGNMENT, align);
	real_glPixelStorei(GL_PACK_SKIP_ROWS, skiprows);
	real_glPixelStorei(GL_PACK_SKIP_PIXELS, skippix);
	for (j = 0; j < h; j++) {
		char *row = (char *)data + (size_t)(j + skiprows) * stride + (size_t)skippix * es;

		for (i = 0; i < w; i++, in += 4) {
			GLint v = rgb_index(in);

			switch (type) {
			case GL_UNSIGNED_BYTE:
			case GL_BYTE:
				((GLubyte *)row)[i] = v;
				break;
			case GL_UNSIGNED_SHORT:
			case GL_SHORT:
				((GLushort *)row)[i] = v;
				break;
			case GL_UNSIGNED_INT:
			case GL_INT:
				((GLuint *)row)[i] = v;
				break;
			case GL_FLOAT:
				((GLfloat *)row)[i] = v;
				break;
			}
		}
	}
	free(buf);
}
