/*
 * libglxsync: IRIX n32 preload library for QuickTransit.
 *
 * IRIX programs mix X and OpenGL drawing in one single-buffered window
 * without glXWaitGL()/glXWaitX(), because on SGI hardware both draw into the
 * same framebuffer.  Current Mesa renders single-buffered windows into a
 * "fake front" buffer and copies the whole buffer to the window on flush, so
 * X drawing made in between is painted over.  Example: the desktop icon panel
 * (fm, iconcatalog, the background) draws icons with GL and their labels with
 * X, then releases the context; the release flush erases every label.
 *
 * Fix: when an X call draws on the drawable of the current GLX context,
 * glXWaitGL() first (fake front -> window) and glXWaitX() after (window ->
 * fake front), which is what the program would have to do itself.  The
 * XSync() before glXWaitX() is needed because QuickTransit's GL runs on its
 * own host X connection: glXWaitX() syncs that one, not the program's, so
 * without it the copy can happen before the server has drawn the X request.
 *
 * Root painters: twilight (and the other "-execute" backgrounds in
 * /usr/lib/X11/system.backgrounds) draw with GL into a full-screen window
 * at the bottom of the stack.  On SGI hardware the desktop icons (fm -b)
 * sit in the overlay planes and the painter shows through; without overlay
 * visuals fm's full-screen window covers it.  fm does show the pixmap named
 * by the root's _SGI_DESKS_BACKGROUND (how 4Dwm publishes image
 * backgrounds), so for a program that sets _SGI_ROOTPAINTER_ID each frame it
 * swaps is read back into a pixmap that becomes the root background and
 * _SGI_DESKS_BACKGROUND.  At most once a second.
 *
 * install-extras.sh preloads it into every n32 program (FU_RLDN32_LIST in
 * /usr/QuickTransit/etc/QuickTransit.conf; see docs/qt-internals.md).
 * GLXSYNC_DEBUG=1 logs.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <dlfcn.h>
#include <X11/Xlib.h>
#include <X11/Xatom.h>
#include <X11/Xutil.h>
#include <GL/glx.h>

static void *x11, *gl;
static GLXDrawable cur;		/* drawable of the current context, or 0 */
static int debug = -1;

static void *real(void **h, const char *lib, const char *name)
{
	void *f;

	if (!*h)
		*h = dlopen(lib, RTLD_LAZY);
	f = *h ? dlsym(*h, name) : NULL;
	if (!f) {
		fprintf(stderr, "glxsync: no %s in %s\n", name, lib);
		abort();
	}
	return f;
}
#define REALX(name)  real(&x11, "libX11.so.1", name)
#define REALGL(name) real(&gl, "libGL.so", name)

static void log_(const char *what, Drawable d)
{
	if (debug < 0)
		debug = getenv("GLXSYNC_DEBUG") != NULL;
	if (debug)
		fprintf(stderr, "glxsync: %s on %lx\n", what, d);
}

/* Bracket an X drawing call on the current GL drawable. */
#define BEFORE(d, name) \
	int sync_ = cur && (Drawable)(d) == (Drawable)cur; \
	if (sync_) { \
		void (*wgl)(void) = REALGL("glXWaitGL"); \
		log_(name, d); \
		wgl(); \
	}
#define AFTER() \
	if (sync_) { \
		void (*wx)(void) = REALGL("glXWaitX"); \
		XSync(d, False); \
		wx(); \
	}

/*
 * glXMakeCurrent, glXSwapBuffers and XChangeProperty are also exported as
 * glxsync_<name>: libfakeci, earlier in the preload list, wraps the same
 * calls, and IRIX rld has no RTLD_NEXT for it to find these.
 */
Bool glxsync_glXMakeCurrent(Display *d, GLXDrawable w, GLXContext c)
{
	Bool (*f)(Display *, GLXDrawable, GLXContext) = REALGL("glXMakeCurrent");
	Bool r = f(d, w, c);

	if (r)
		cur = c ? w : 0;
	return r;
}

Bool glXMakeCurrent(Display *d, GLXDrawable w, GLXContext c)
{
	return glxsync_glXMakeCurrent(d, w, c);
}

Bool glXMakeContextCurrent(Display *d, GLXDrawable w, GLXDrawable rd, GLXContext c)
{
	Bool (*f)(Display *, GLXDrawable, GLXDrawable, GLXContext) = REALGL("glXMakeContextCurrent");
	Bool r = f(d, w, rd, c);

	if (r)
		cur = c ? w : 0;
	return r;
}

/* XN(ret, name, params, args, drawable): wrapper for an X drawing call */
#define XN(ret, name, params, args, dr) \
ret name params \
{ \
	ret (*f) params = REALX(#name); \
	ret ret_; \
	BEFORE(dr, #name) \
	ret_ = f args; \
	AFTER() \
	return ret_; \
}
#define XV(name, params, args, dr) \
void name params \
{ \
	void (*f) params = REALX(#name); \
	BEFORE(dr, #name) \
	f args; \
	AFTER() \
}

XV(XmbDrawString, (Display *d, Drawable w, XFontSet fs, GC gc, int x, int y, const char *s, int n),
	(d, w, fs, gc, x, y, s, n), w)
XV(XmbDrawImageString, (Display *d, Drawable w, XFontSet fs, GC gc, int x, int y, const char *s, int n),
	(d, w, fs, gc, x, y, s, n), w)
XV(XwcDrawString, (Display *d, Drawable w, XFontSet fs, GC gc, int x, int y, const wchar_t *s, int n),
	(d, w, fs, gc, x, y, s, n), w)
XN(int, XDrawString, (Display *d, Drawable w, GC gc, int x, int y, const char *s, int n),
	(d, w, gc, x, y, s, n), w)
XN(int, XDrawImageString, (Display *d, Drawable w, GC gc, int x, int y, const char *s, int n),
	(d, w, gc, x, y, s, n), w)
XN(int, XFillRectangle, (Display *d, Drawable w, GC gc, int x, int y, unsigned int wd, unsigned int ht),
	(d, w, gc, x, y, wd, ht), w)
XN(int, XFillRectangles, (Display *d, Drawable w, GC gc, XRectangle *r, int n),
	(d, w, gc, r, n), w)
XN(int, XDrawRectangle, (Display *d, Drawable w, GC gc, int x, int y, unsigned int wd, unsigned int ht),
	(d, w, gc, x, y, wd, ht), w)
XN(int, XDrawLine, (Display *d, Drawable w, GC gc, int x1, int y1, int x2, int y2),
	(d, w, gc, x1, y1, x2, y2), w)
XN(int, XDrawLines, (Display *d, Drawable w, GC gc, XPoint *p, int n, int m),
	(d, w, gc, p, n, m), w)
XN(int, XDrawSegments, (Display *d, Drawable w, GC gc, XSegment *s, int n),
	(d, w, gc, s, n), w)
XN(int, XFillPolygon, (Display *d, Drawable w, GC gc, XPoint *p, int n, int sh, int m),
	(d, w, gc, p, n, sh, m), w)
XN(int, XCopyArea, (Display *d, Drawable s, Drawable w, GC gc, int sx, int sy, unsigned int wd, unsigned int ht, int dx, int dy),
	(d, s, w, gc, sx, sy, wd, ht, dx, dy), w)
XN(int, XPutImage, (Display *d, Drawable w, GC gc, XImage *im, int sx, int sy, int dx, int dy, unsigned int wd, unsigned int ht),
	(d, w, gc, im, sx, sy, dx, dy, wd, ht), w)

/* Root painters (see the top of the file) */
static Display *pdpy;
static Window painter;		/* window named by _SGI_ROOTPAINTER_ID */

int glxsync_XChangeProperty(Display *d, Window w, Atom p, Atom t, int fmt, int mode, const unsigned char *data, int n)
{
	int (*f)(Display *, Window, Atom, Atom, int, int, const unsigned char *, int) = REALX("XChangeProperty");

	if (w == DefaultRootWindow(d) && fmt == 32 && n == 1 &&
	    p == XInternAtom(d, "_SGI_ROOTPAINTER_ID", False)) {
		pdpy = d;
		painter = *(const long *)data;
		log_("root painter", painter);
	}
	return f(d, w, p, t, fmt, mode, data, n);
}

int XChangeProperty(Display *d, Window w, Atom p, Atom t, int fmt, int mode, const unsigned char *data, int n)
{
	return glxsync_XChangeProperty(d, w, p, t, fmt, mode, data, n);
}

static int shift(unsigned long m)
{
	int s = 0;

	while (m && !(m & 1)) {
		m >>= 1;
		s++;
	}
	return s;
}

/* Copy the back buffer of the painter window to the root background. */
static void paint_root(Display *d, Window w)
{
	static Pixmap pix;
	static GC gc;
	static XImage *im;
	static unsigned char *rgba;
	static time_t last;
	void (*readbuf)(GLenum) = REALGL("glReadBuffer");
	void (*getiv)(GLenum, GLint *) = REALGL("glGetIntegerv");
	void (*store)(GLenum, GLint) = REALGL("glPixelStorei");
	void (*readpix)(GLint, GLint, GLsizei, GLsizei, GLenum, GLenum, GLvoid *) = REALGL("glReadPixels");
	Window root = DefaultRootWindow(d);
	XWindowAttributes ra;
	Window r;
	int x, y, rs, gs, bs;
	unsigned int ww, wh, b, dep;
	GLint rb, pa;

	if (time(NULL) - last < 1 ||
	    !XGetGeometry(d, w, &r, &x, &y, &ww, &wh, &b, &dep) ||
	    !XGetWindowAttributes(d, root, &ra) || ra.visual->class != TrueColor)
		return;
	last = time(NULL);
	if (!im || im->width != (int)ww || im->height != (int)wh) {
		if (im)
			XDestroyImage(im);	/* frees its data */
		if (pix)
			XFreePixmap(d, pix);
		free(rgba);
		rgba = malloc(ww * wh * 4);
		im = XCreateImage(d, ra.visual, ra.depth, ZPixmap, 0, malloc(ww * wh * 4), ww, wh, 32, 0);
		pix = XCreatePixmap(d, root, ww, wh, ra.depth);
		if (!gc)
			gc = XCreateGC(d, pix, 0, NULL);
		if (!rgba || !im || !im->data || im->bits_per_pixel != 32) {
			painter = 0;
			return;
		}
	}
	log_("root paint", w);
	getiv(GL_READ_BUFFER, &rb);
	getiv(GL_PACK_ALIGNMENT, &pa);
	readbuf(GL_BACK);
	store(GL_PACK_ALIGNMENT, 4);
	readpix(0, 0, ww, wh, GL_RGBA, GL_UNSIGNED_BYTE, rgba);
	store(GL_PACK_ALIGNMENT, pa);
	readbuf(rb);

	/* GL rows are bottom-up; write pixels in the image's byte order */
	rs = shift(ra.visual->red_mask);
	gs = shift(ra.visual->green_mask);
	bs = shift(ra.visual->blue_mask);
	for (y = 0; y < (int)wh; y++) {
		const unsigned char *s = rgba + (wh - 1 - y) * ww * 4;
		unsigned char *o = (unsigned char *)im->data + y * im->bytes_per_line;
		for (x = 0; x < (int)ww; x++, s += 4, o += 4) {
			unsigned long px = (unsigned long)s[0] << rs | (unsigned long)s[1] << gs | (unsigned long)s[2] << bs;
			if (im->byte_order == LSBFirst) {
				o[0] = px; o[1] = px >> 8; o[2] = px >> 16; o[3] = px >> 24;
			} else {
				o[3] = px; o[2] = px >> 8; o[1] = px >> 16; o[0] = px >> 24;
			}
		}
	}
	XPutImage(d, pix, gc, im, 0, 0, 0, 0, ww, wh);
	XSetWindowBackgroundPixmap(d, root, pix);
	XClearWindow(d, root);
	XChangeProperty(d, root, XInternAtom(d, "_SGI_DESKS_BACKGROUND", False), XA_PIXMAP, 32,
			PropModeReplace, (unsigned char *)&pix, 1);
	XFlush(d);
}

void glxsync_glXSwapBuffers(Display *d, GLXDrawable w)
{
	void (*f)(Display *, GLXDrawable) = REALGL("glXSwapBuffers");

	if (painter && d == pdpy && (Window)w == painter)
		paint_root(d, w);
	f(d, w);
}

void glXSwapBuffers(Display *d, GLXDrawable w)
{
	glxsync_glXSwapBuffers(d, w);
}
