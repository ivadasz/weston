/*
 * Copyright © 2010-2011 Benjamin Franzke
 * Copyright © 2012 Intel Corporation
 *
 * Permission to use, copy, modify, distribute, and sell this software and
 * its documentation for any purpose is hereby granted without fee, provided
 * that the above copyright notice appear in all copies and that both that
 * copyright notice and this permission notice appear in supporting
 * documentation, and that the name of the copyright holders not be used in
 * advertising or publicity pertaining to distribution of the software
 * without specific, written prior permission.  The copyright holders make
 * no representations about the suitability of this software for any
 * purpose.  It is provided "as is" without express or implied warranty.
 *
 * THE COPYRIGHT HOLDERS DISCLAIM ALL WARRANTIES WITH REGARD TO THIS
 * SOFTWARE, INCLUDING ALL IMPLIED WARRANTIES OF MERCHANTABILITY AND
 * FITNESS, IN NO EVENT SHALL THE COPYRIGHT HOLDERS BE LIABLE FOR ANY
 * SPECIAL, INDIRECT OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES WHATSOEVER
 * RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN ACTION OF
 * CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF OR IN
 * CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */

#include "config.h"

#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <sys/time.h>
#include <linux/input.h>

#include <rfb/rfbconfig.h>
#include <rfb/rfb.h>
#include <rfb/keysym.h>

#include "compositor.h"
#include "pixman-renderer.h"

#define DEFAULT_AXIS_STEP_DISTANCE wl_fixed_from_int(5)

struct vnc_output;
struct input_event_item {
	int type;
	struct vnc_output *output;
	rfbBool down;
	rfbKeySym keySym;
	uint32_t time;
	int buttonMask;
	int xabs;
	int yabs;
	struct wl_list link;
};

struct frame_finished_item {
	struct timespec ts;
	struct wl_list link;
};

struct vnc_compositor {
	struct weston_compositor base;
	struct weston_seat core_seat;
	struct wl_event_source *input_source;
	int ptrx, ptry, ptrmask;		/* current mouse parameters */
	/* XXX put mouse and keyboard events into the same queue */
	struct wl_list vnc_input_list;	/* mouse input queue */
	uint64_t input_queue_length;		/* count length of queue */
	pthread_mutex_t input_mtx;
	pthread_mutex_t finish_mtx;

	struct wl_list vnc_frame_list;	/* frame finished queue */
	pthread_mutex_t frame_mtx;
};

struct vnc_output {
	struct weston_output base;
	struct weston_mode mode;
	struct wl_event_source *finish_frame_timer;
	rfbScreenInfoPtr vncserver;
	pixman_image_t *shadow_surface, *surface_a, *surface_b;
	struct vnc_compositor *c;
	void *fb_a, *fb_b;
	uint32_t repaints, vncdisplays;
	struct weston_plane cursor_plane;
	struct weston_view *cursor_view;
	uint32_t cursor_buf[64*64];
	int cursorChanged;
	int cx, cy;
	int cursor_width, cursor_height;
};

static int finish_frame_handler(void *data);
static int frame_handler_cnt = 0;

static void
vnc_output_start_repaint_loop(struct weston_output *output)
{
//	struct vnc_output *vncoutput = (struct vnc_output *)output;
	struct timespec ts;

	clock_gettime(output->compositor->presentation_clock, &ts);
	frame_handler_cnt++;
	finish_frame_handler(output);
//	weston_output_finish_frame(output, &ts);

//	fprintf(stderr, "%s: called\n", __func__);
}

static int
finish_frame_handler(void *data)
{
	struct weston_output *output = (struct weston_output *)data;
	struct vnc_output *vncoutput = (struct vnc_output *)data;
	struct frame_finished_item *it, *next;
	struct timespec ts;
	static struct timespec lastts;
	static int initted = 0;
	uint64_t lastms, ms;

	clock_gettime(output->compositor->presentation_clock, &ts);
	if (initted) {
		lastms = lastts.tv_sec * 1000 + lastts.tv_nsec / 1000000;
		ms = ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
	}
	if (initted && lastms + 500 < ms) {
//		fprintf(stderr, "large time jump of %jums\n",
//		    ms - lastms);
		frame_handler_cnt++;
	}

	pthread_mutex_lock(&vncoutput->c->frame_mtx);
	if (frame_handler_cnt <= 0 && wl_list_empty(&vncoutput->c->vnc_frame_list)) {
		frame_handler_cnt = 0;
		pthread_mutex_unlock(&vncoutput->c->frame_mtx);
		goto wasempty;
	}
	wl_list_for_each_reverse_safe(it, next, &vncoutput->c->vnc_frame_list, link) {
		frame_handler_cnt++;
		ts = it->ts;
		free(it);
	}
	frame_handler_cnt--;
	weston_output_finish_frame(output, &ts);
	lastts = ts;
	initted = 1;
	wl_list_init(&vncoutput->c->vnc_frame_list);
	pthread_mutex_unlock(&vncoutput->c->frame_mtx);
wasempty:

	wl_event_source_timer_update(vncoutput->finish_frame_timer, 50);

	return 1;

#if 0
	clock_gettime(output->compositor->presentation_clock, &ts);
	weston_output_finish_frame(output, &ts);
//	wl_event_source_timer_update(vncoutput->finish_frame_timer, 40);

//	vnc_output_start_repaint_loop(data);
#endif

	return 1;
}

static void vnc_output_set_cursor(struct vnc_output *output);

static int
vnc_output_repaint(struct weston_output *base,
		   pixman_region32_t *damage)
{
	struct vnc_output *output = (struct vnc_output *) base;
	struct weston_compositor *ec = output->base.compositor;
	pixman_box32_t *rects;
	int nrects, i, x1, y1, x2, y2;

//	fprintf(stderr, "%s: called\n", __func__);

	/* Repaint the damaged region onto the back buffer. */
	pthread_mutex_lock(&output->c->finish_mtx);
	pixman_renderer_output_set_buffer(base, output->shadow_surface);
	pthread_mutex_unlock(&output->c->finish_mtx);
	ec->renderer->repaint_output(base, damage);
	output->repaints++;
//	fprintf(stderr, "repaints: %u\n", output->repaints);

//	vnc_output_set_cursor(output);

	rects = pixman_region32_rectangles(damage, &nrects);
	for (i = 0; i < nrects; i++) {
		x1 = rects[i].x1;
		x2 = rects[i].x2;
		y1 = rects[i].y1;
		y2 = rects[i].y2;
//		fprintf(stderr, "markasmodified rect %dx%d,%dx%d\n", x1, y1, x2, y2);
		rfbMarkRectAsModified(output->vncserver, x1, y1, x2, y2);
	}

	pixman_region32_subtract(&ec->primary_plane.damage,
				 &ec->primary_plane.damage, damage);

	return 0;
}

static void
vnc_output_destroy(struct weston_output *output_base)
{
	struct vnc_output *output = (struct vnc_output *) output_base;

	rfbScreenCleanup(output->vncserver);
	weston_plane_release(&output->cursor_plane);
	wl_event_source_remove(output->finish_frame_timer);
	free(output);

	return;
}

static void
vnc_copy_cursor(struct vnc_output *output)
{
	struct weston_view *ev = output->cursor_view;
	struct weston_buffer *buffer;
	unsigned char *s;
	int i, j;
	uint32_t *buf = output->cursor_buf;
	uint32_t stride;
	int changed = 0;

	buffer = ev->surface->buffer_ref.buffer;
	pixman_region32_fini(&output->cursor_plane.damage);
	pixman_region32_init(&output->cursor_plane.damage);
	stride = wl_shm_buffer_get_stride(buffer->shm_buffer);
	s = wl_shm_buffer_get_data(buffer->shm_buffer);
	wl_shm_buffer_begin_access(buffer->shm_buffer);
	for (i = 0; i < ev->surface->height; i++) {
		for (j = 0; j < ev->surface->width; j++) {
			if (buf[i*ev->surface->width + j] != ((uint32_t *)s)[i*(stride/4) + j]) {
				buf[i*ev->surface->width + j] = ((uint32_t *)s)[i*(stride/4) + j];
				changed = 1;
			}
		}
	}
	wl_shm_buffer_end_access(buffer->shm_buffer);
	if (output->cursor_width != 1 || output->cursor_height != 1)
		changed = 1;
	output->cursorChanged = output->cursorChanged || changed;
	output->cursor_width = ev->surface->width;
	output->cursor_height = ev->surface->height;
	if (output->cursor_width > 64 || output->cursor_height > 64)
		fprintf(stderr, "%s: cursor_width: %d, cursor_height: %d\n", __func__, output->cursor_width, output->cursor_height);
}

static void
vnc_hide_cursor(struct vnc_output *output)
{
	uint32_t *buf = output->cursor_buf;
	int changed = 0;

	if (buf[0] != 0) {
		buf[0] = 0;
		changed = 1;
	}
	if (output->cursor_width != 1 || output->cursor_height != 1)
		changed = 1;
	output->cursorChanged = output->cursorChanged || changed;
	output->cursor_width = 1;
	output->cursor_height = 1;
}

static struct weston_plane *
vnc_prepare_cursor_view(struct weston_output *output_base,
			struct weston_view *ev)
{
#if 0
	struct weston_buffer_viewport *viewport = &ev->surface->buffer_viewport;
#endif
	struct vnc_output *output = (struct vnc_output *) output_base;

#if 0
	if (output->base.transform != WL_OUTPUT_TRANSFORM_NORMAL)
		return NULL;
	if (viewport->buffer.scale != output_base->current_scale)
		return NULL;
#endif
//	if (output->cursor_view)
//		return NULL;
//	if (ev->output_mask != (1u << output_base->id))
//		return NULL;
//	if (ev->surface->buffer_ref.buffer == NULL ||
	if (
//	    wl_shm_buffer_get(ev->surface->buffer_ref.buffer->resource) ||
	    ev->surface->width > 64 || ev->surface->height > 64)
		return NULL;

	output->cursor_view = ev;

	if (ev->surface->buffer_ref.buffer != NULL &&
	   wl_shm_buffer_get(ev->surface->buffer_ref.buffer->resource)) {
//		fprintf(stderr, "%s: copying cursor\n", __func__);
		vnc_copy_cursor(output);
	}

	return &output->cursor_plane;
}

static void
vnc_output_set_cursor(struct vnc_output *output)
{
	struct weston_view *ev = output->cursor_view;
	int i;
	uint32_t *buf = output->cursor_buf;
	rfbCursorPtr cursor;

	output->cursor_view = NULL;
	if (ev == NULL && !output->cursorChanged) {
		return;
	}

	LOCK(output->vncserver->cursorMutex);
	if (output->cursorChanged) {
		output->cursorChanged = 0;
		char *src = malloc(output->cursor_width*output->cursor_height);
		char *mask = malloc(output->cursor_width*output->cursor_height);
//		fprintf(stderr, "updating vnc cursor\n");
		for (i = 0; i < output->cursor_width*output->cursor_height; i++) {
			mask[i] = ((buf[i] & 0xff000000) >> 24) == 0xff ? 'x' : ' ';
		}
		cursor = rfbMakeXCursor(output->cursor_width, output->cursor_height, src, mask);
		cursor->richSource = (uint8_t *)buf;
	       	cursor->cleanupSource = TRUE;
	       	cursor->cleanupMask = TRUE;
	       	cursor->cleanupRichSource = FALSE;
		rfbSetCursor(output->vncserver, cursor);
	}
	if (ev != NULL) {
		output->cx = ev->geometry.x;
		output->cy = ev->geometry.y;
	} else {
		output->cx = output->vncserver->cursorX;
		output->cy = output->vncserver->cursorY;
	}
	output->cursor_plane.x = output->cx;
	output->cursor_plane.y = output->cy;
	if (output->vncserver->cursor != NULL) {
		if (output->vncserver->cursorX < output->cx)
			output->vncserver->cursor->xhot = 0;
		else
			output->vncserver->cursor->xhot = output->vncserver->cursorX - output->cx;
		if (output->vncserver->cursorY < output->cy)
			output->vncserver->cursor->yhot = 0;
		else
			output->vncserver->cursor->yhot = output->vncserver->cursorY - output->cy;
	}
	UNLOCK(output->vncserver->cursorMutex);
}

static void
vnc_assign_planes(struct weston_output *output)
{
	struct vnc_compositor *c =
	    (struct vnc_compositor *)output->compositor;
	struct vnc_output *vncoutput = (struct vnc_output *)output;
	struct weston_view *ev, *next;
	struct weston_plane *next_plane;
	int i, k;

	i = 0;
	k = 0;
	wl_list_for_each_safe(ev, next, &c->base.view_list, link) {
		i++;
		next_plane = NULL;
		if (next_plane == NULL && k == 0) {
			next_plane = vnc_prepare_cursor_view(output, ev);
			if (next_plane != NULL)
				k++;
		}
		if (next_plane == NULL) {
			next_plane = &c->base.primary_plane;
		} else if (vncoutput->cursor_view == ev) {
			weston_view_move_to_plane(ev, &vncoutput->cursor_plane);
//			fprintf(stderr, "view with size %dx%d\n", ev->surface->width, ev->surface->height);
			continue;
		} else {
//			fprintf(stderr, "using cursor_plane\n");
		}
//		if (ev->surface->width <= 64 && ev->surface->height <= 64) {
//			fprintf(stderr, "view with size %dx%d\n", ev->surface->width, ev->surface->height);
//		}
		weston_view_move_to_plane(ev, next_plane);
	}
	if (vncoutput->cursor_view == NULL) {
//		fprintf(stderr, "%s: hiding cursor\n", __func__);
		vnc_hide_cursor(vncoutput);
	}
	vnc_output_set_cursor(vncoutput);
}

static void
vnc_display_event(struct _rfbClientRec *cl)
{
#if 0
	struct vnc_output *output = cl->screen->screenData;
#endif

//	fprintf(stderr, "%s: called\n", __func__);

#if 0
	pthread_mutex_lock(&output->c->finish_mtx);
	if (output->shadow_surface == output->surface_a)
		output->shadow_surface = output->surface_b;
	else
		output->shadow_surface = output->surface_a;
	pthread_mutex_unlock(&output->c->finish_mtx);
#endif

#if 0
	if (output->vncserver->cursor != NULL) {
		output->vncserver->cursor->xhot = output->vncserver->cursorX-output->cx;
		output->vncserver->cursor->yhot = output->vncserver->cursorY-output->cy;
	}
#endif
}

static void
vnc_displayfinished_event(struct _rfbClientRec *cl, int result)
{
	struct vnc_output *output = cl->screen->screenData;
	struct frame_finished_item *it;

#if 0
	if (output->vncserver->frameBuffer == output->fb_a)
		output->vncserver->frameBuffer = output->fb_b;
	else
		output->vncserver->frameBuffer = output->fb_a;
#endif

	output->vncdisplays++;
//	fprintf(stderr, "vncdisplays: %u\n", output->vncdisplays);

//	fprintf(stderr, "%s: called\n", __func__);

	it = zalloc(sizeof(struct frame_finished_item));
	if (it == NULL) {
		perror("zalloc");
		return;
	}

	clock_gettime(output->base.compositor->presentation_clock, &it->ts);
	pthread_mutex_lock(&output->c->frame_mtx);
	wl_list_insert(&output->c->vnc_frame_list, &it->link);
	pthread_mutex_unlock(&output->c->frame_mtx);
	wl_event_source_activate(output->finish_frame_timer);
}

static void
vnc_ptr_event(int buttonMask, int x, int y, struct _rfbClientRec *cl)
{
	struct vnc_output *output = cl->screen->screenData;
	struct vnc_compositor *c = output->c;
	struct input_event_item *it;

//	fprintf(stderr, "%s: buttonMask=%d, x=%d, y=%d\n", __func__,
//	    buttonMask, x, y);

	it = zalloc(sizeof(struct input_event_item));
	if (it == NULL) {
		perror("zalloc");
		return;
	}

	it->type = 1;
	it->output = output;
	it->time = weston_compositor_get_time();
	it->buttonMask = buttonMask;
	it->xabs = x;
	it->yabs = y;

	pthread_mutex_lock(&c->input_mtx);
	wl_list_insert(&c->vnc_input_list, &it->link);
	c->input_queue_length++;
	pthread_mutex_unlock(&c->input_mtx);

	if (c->input_queue_length > 10000) {
		fprintf(stderr, "%s: excessive mouse input queue length: %jd entries\n", __func__, c->input_queue_length);
		if (*(volatile uint64_t *)&c->input_queue_length > 10000) {
			wl_event_source_activate(c->input_source);
			pthread_yield();
		}
	}

//	fprintf(stderr, "%s: x=%d, y=%d, cx=%d, cy=%d\n", __func__, x, y, output->cx, output->cy);
	rfbDefaultPtrAddEvent(buttonMask,x,y,cl);

	wl_event_source_activate(c->input_source);
}

static void
vnc_kbd_event(rfbBool down, rfbKeySym keySym, struct _rfbClientRec *cl)
{
	struct vnc_output *output = cl->screen->screenData;
	struct vnc_compositor *c = output->c;
	struct input_event_item *it;

	if (down) {
		fprintf(stderr, "%s: pressed sym: 0x%x\n", __func__, keySym);
	} else {
		fprintf(stderr, "%s: released sym: 0x%x\n", __func__, keySym);
	}

	it = zalloc(sizeof(struct input_event_item));
	if (it == NULL) {
		perror("zalloc");
		return;
	}

	it->type = 2;
	it->output = output;
	it->time = weston_compositor_get_time();
	it->down = down;
	it->keySym = keySym;

	pthread_mutex_lock(&c->input_mtx);
	wl_list_insert(&c->vnc_input_list, &it->link);
	c->input_queue_length++;
	pthread_mutex_unlock(&c->input_mtx);

	if (c->input_queue_length > 10000) {
		fprintf(stderr, "%s: excessive input queue length: %jd entries\n", __func__, c->input_queue_length);
		if (*(volatile uint64_t *)&c->input_queue_length > 10000) {
			wl_event_source_activate(c->input_source);
			pthread_yield();
		}
	}

	wl_event_source_activate(c->input_source);
}

static int
vnc_compositor_create_output(struct vnc_compositor *c, int width, int height, char *listen, int port)
{
	struct vnc_output *output;
	struct wl_event_loop *loop;
	in_addr_t iface;

	output = zalloc(sizeof *output);
	if (output == NULL)
		return -1;

	output->c = c;
	output->vncserver = rfbGetScreen(NULL, NULL, width, height, 8, 3, 4);
	output->vncserver->deferUpdateTime = 10;
	output->vncserver->screenData = output;
	output->fb_a = calloc(width * height, 4);
	output->fb_b = calloc(width * height, 4);
	/* XXX clean up on calloc failure */
	output->vncserver->frameBuffer = output->fb_a;
	output->vncserver->displayHook = vnc_display_event;
	output->vncserver->displayFinishedHook = vnc_displayfinished_event;
	output->vncserver->ptrAddEvent = vnc_ptr_event;
	output->vncserver->kbdAddEvent = vnc_kbd_event;
	output->vncserver->autoPort = FALSE;
	output->vncserver->port = port;
	if (rfbStringToAddr(listen, &iface)) {
		output->vncserver->listenInterface = iface;
	} else if (rfbStringToAddr("localhost", &iface)) {
		output->vncserver->listenInterface = iface;
	}
	output->vncserver->listen6Interface = "::1";

	output->surface_a =
		pixman_image_create_bits(PIXMAN_x8b8g8r8, width, height,
					 (uint32_t *)output->fb_a, width * 4);
	if (output->surface_a == NULL) {
		weston_log("Failed to create surface for frame buffer.\n");
		/* XXX clean up correctly */
		return -1;
	}
	output->surface_b =
		pixman_image_create_bits(PIXMAN_x8b8g8r8, width, height,
					 (uint32_t *)output->fb_b, width * 4);
	if (output->surface_b == NULL) {
		weston_log("Failed to create surface for frame buffer.\n");
		/* XXX clean up correctly */
		return -1;
	}
	output->shadow_surface = output->surface_a;

	output->repaints = 0;
	output->vncdisplays = 0;

	output->cursorChanged = 0;
	output->cx = 0;
	output->cy = 0;

	output->mode.flags =
		WL_OUTPUT_MODE_CURRENT | WL_OUTPUT_MODE_PREFERRED;
	output->mode.width = width;
	output->mode.height = height;
	output->mode.refresh = 33;
	wl_list_init(&output->base.mode_list);
	wl_list_insert(&output->base.mode_list, &output->mode.link);

	output->base.current_mode = &output->mode;
	weston_output_init(&output->base, &c->base, 0, 0, width, height,
			   WL_OUTPUT_TRANSFORM_NORMAL, 1);

	output->base.make = "weston";
	output->base.model = "vnc";

	if (pixman_renderer_output_create(&output->base) < 0) {
		return -1;
	}

	loop = wl_display_get_event_loop(c->base.wl_display);
	output->finish_frame_timer =
		wl_event_loop_add_timer(loop, finish_frame_handler, output);

	output->base.start_repaint_loop = vnc_output_start_repaint_loop;
	output->base.repaint = vnc_output_repaint;
	output->base.destroy = vnc_output_destroy;
	output->base.assign_planes = vnc_assign_planes;
	output->base.set_backlight = NULL;
	output->base.set_dpms = NULL;
	output->base.switch_mode = NULL;

	weston_plane_init(&output->cursor_plane, &c->base, 0, 0);

	weston_compositor_stack_plane(&c->base, &output->cursor_plane, NULL);

	wl_list_insert(c->base.output_list.prev, &output->base.link);

	rfbInitServer(output->vncserver);
	rfbRunEventLoop(output->vncserver, -1, 1);

	return 0;
}

static int
vnc_pass_mouse_events(struct vnc_compositor *c, struct input_event_item *it)
{
	wl_fixed_t wl_x, wl_y;
	int nm;
	int clicks = 0;

	static uint32_t lasttime;
	static int prevx = 0, prevy = 0;
	static int prevmask = 0;
	static int lazymotion = 0;

	if (it == NULL && lazymotion) {
		wl_x = wl_fixed_from_int(prevx);
		wl_y = wl_fixed_from_int(prevy);
		notify_motion_absolute(&c->core_seat, lasttime, wl_x, wl_y);
		clicks++;
		lazymotion = 0;
	}
	if (it == NULL)
		return clicks;

	nm = it->buttonMask;
	lasttime = it->time;

	if (prevx != it->xabs || prevy != it->yabs) {
//		fprintf(stderr, "moving from %dx%d to %dx%d\n", prevx, prevy, it->xabs, it->yabs);
		lazymotion = 1;
	}
	prevx = it->xabs;
	prevy = it->yabs;
	if (prevmask != nm) {
		if (lazymotion) {
			wl_x = wl_fixed_from_int(prevx);
			wl_y = wl_fixed_from_int(prevy);
			notify_motion_absolute(&c->core_seat, it->time, wl_x, wl_y);
			lazymotion = 0;
			clicks++;
		}
//		fprintf(stderr, "setting mask from %d to %d\n", prevmask, nm);
		if ((prevmask & 1) != (nm & 1)) {
			notify_button(&c->core_seat,
			    it->time, BTN_LEFT,
			    (nm & 1) ? WL_POINTER_BUTTON_STATE_PRESSED :
			               WL_POINTER_BUTTON_STATE_RELEASED);
			clicks++;
		}
		if ((prevmask & 2) != (nm & 2)) {
			notify_button(&c->core_seat,
			    it->time, BTN_MIDDLE,
			    (nm & 2) ? WL_POINTER_BUTTON_STATE_PRESSED :
			               WL_POINTER_BUTTON_STATE_RELEASED);
			clicks++;
		}
		if ((prevmask & 4) != (nm & 4)) {
			notify_button(&c->core_seat,
			    it->time, BTN_RIGHT,
			    (nm & 4) ? WL_POINTER_BUTTON_STATE_PRESSED :
			               WL_POINTER_BUTTON_STATE_RELEASED);
			clicks++;
		}
		if ((prevmask & 8) != (nm & 8)) {
			notify_axis(&c->core_seat,
			    it->time, WL_POINTER_AXIS_VERTICAL_SCROLL,
			    -DEFAULT_AXIS_STEP_DISTANCE);
			clicks++;
		}
		if ((prevmask & 16) != (nm & 16)) {
			notify_axis(&c->core_seat,
			    it->time, WL_POINTER_AXIS_VERTICAL_SCROLL,
			    DEFAULT_AXIS_STEP_DISTANCE);
			clicks++;
		}
		prevmask = nm;
	}
	return clicks;
}

static uint32_t
vnc_keysym_to_key(int sym)
{
	uint32_t key;

	if (sym == XK_BackSpace)
		key = KEY_BACKSPACE;
	else if (sym == XK_Tab)
		key = KEY_TAB;
	else if (sym == XK_Linefeed)
		key = KEY_LINEFEED;
	else if (sym == XK_Clear)
		key = KEY_CLEAR;
	else if (sym == XK_Return)
		key = KEY_ENTER;
	/* XXX */
	else if (sym == XK_Escape)
		key = KEY_ESC;
	/* XXX */
	else if (sym == XK_space)
		key = KEY_SPACE;
	/* XXX */
	else if (sym == XK_parenleft)
		key = KEY_8;
	else if (sym == XK_parenright)
		key = KEY_9;
//	else if (sym == XK_asterisk)
//		key = KEY_XXX;
//	else if (sym == XK_plus)
//		key = KEY_XXX;
	else if (sym == XK_comma)
		key = KEY_COMMA;
	else if (sym == XK_minus)
		key = KEY_SLASH;
	else if (sym == XK_period)
		key = KEY_DOT;
	else if (sym == XK_slash)
		key = KEY_7;
	else if (sym == XK_0)
		key = KEY_0;
	else if (sym >= XK_1 && sym <= XK_9)
		key = KEY_1 + (sym - XK_1);
	else if (sym == XK_colon)
		key = KEY_DOT;
	else if (sym == XK_semicolon)
		key = KEY_COMMA;
//	else if (sym == XK_less)
//		key = KEY_XXX;
	else if (sym == XK_equal)
		key = KEY_0;
//	else if (sym == XK_greater)
//		key = KEY_LESS;
	else if (sym == XK_question)
		key = KEY_MINUS;
	else if (sym == XK_at)
		key = KEY_Q;
	/* XXX */
	else if (sym == XK_Shift_L)
		key = KEY_LEFTSHIFT;
	else if (sym == XK_Shift_R)
		key = KEY_RIGHTSHIFT;
	else if (sym == XK_Control_L)
		key = KEY_LEFTCTRL;
	else if (sym == XK_Control_R)
		key = KEY_RIGHTCTRL;
	/* XXX */
	else if (sym == XK_Meta_L)
		key = KEY_LEFTMETA;
	else if (sym == XK_Meta_R)
		key = KEY_RIGHTMETA;
	else if (sym == XK_Alt_L)
		key = KEY_LEFTALT;
	else if (sym == XK_Alt_R)
		key = KEY_RIGHTALT;
	else if (sym == XK_Super_L)
		key = KEY_LEFTMETA;
	else if (sym == XK_Super_R)
		key = KEY_RIGHTMETA;
	/* XXX */
	else if (sym == XK_A)
		key = KEY_A;
	else if (sym == XK_B)
		key = KEY_B;
	else if (sym == XK_C)
		key = KEY_C;
	else if (sym == XK_D)
		key = KEY_D;
	else if (sym == XK_E)
		key = KEY_E;
	else if (sym == XK_F)
		key = KEY_F;
	else if (sym == XK_G)
		key = KEY_G;
	else if (sym == XK_H)
		key = KEY_H;
	else if (sym == XK_I)
		key = KEY_I;
	else if (sym == XK_J)
		key = KEY_J;
	else if (sym == XK_K)
		key = KEY_K;
	else if (sym == XK_L)
		key = KEY_L;
	else if (sym == XK_M)
		key = KEY_M;
	else if (sym == XK_N)
		key = KEY_N;
	else if (sym == XK_O)
		key = KEY_O;
	else if (sym == XK_P)
		key = KEY_P;
	else if (sym == XK_Q)
		key = KEY_Q;
	else if (sym == XK_R)
		key = KEY_R;
	else if (sym == XK_S)
		key = KEY_S;
	else if (sym == XK_T)
		key = KEY_T;
	else if (sym == XK_U)
		key = KEY_U;
	else if (sym == XK_V)
		key = KEY_V;
	else if (sym == XK_W)
		key = KEY_W;
	else if (sym == XK_X)
		key = KEY_X;
	else if (sym == XK_Y)
		key = KEY_Z;
	else if (sym == XK_Z)
		key = KEY_Y;
	else if (sym == XK_bracketleft)
		key = KEY_8;
	else if (sym == XK_backslash)
		key = KEY_MINUS;
	else if (sym == XK_bracketright)
		key = KEY_9;
//	else if (sym == XK_asciicircum)
//		key = KEY_XXX;
	else if (sym == XK_underscore)
		key = KEY_SLASH;
	else if (sym == XK_grave)
		key = KEY_GRAVE;
	/* XXX */
	else if (sym == XK_a)
		key = KEY_A;
	else if (sym == XK_b)
		key = KEY_B;
	else if (sym == XK_c)
		key = KEY_C;
	else if (sym == XK_d)
		key = KEY_D;
	else if (sym == XK_e)
		key = KEY_E;
	else if (sym == XK_f)
		key = KEY_F;
	else if (sym == XK_g)
		key = KEY_G;
	else if (sym == XK_h)
		key = KEY_H;
	else if (sym == XK_i)
		key = KEY_I;
	else if (sym == XK_j)
		key = KEY_J;
	else if (sym == XK_k)
		key = KEY_K;
	else if (sym == XK_l)
		key = KEY_L;
	else if (sym == XK_m)
		key = KEY_M;
	else if (sym == XK_n)
		key = KEY_N;
	else if (sym == XK_o)
		key = KEY_O;
	else if (sym == XK_p)
		key = KEY_P;
	else if (sym == XK_q)
		key = KEY_Q;
	else if (sym == XK_r)
		key = KEY_R;
	else if (sym == XK_s)
		key = KEY_S;
	else if (sym == XK_t)
		key = KEY_T;
	else if (sym == XK_u)
		key = KEY_U;
	else if (sym == XK_v)
		key = KEY_V;
	else if (sym == XK_w)
		key = KEY_W;
	else if (sym == XK_x)
		key = KEY_X;
	else if (sym == XK_y)
		key = KEY_Z;
	else if (sym == XK_z)
		key = KEY_Y;
	/* XXX */
	else if (sym == XK_ssharp)
		key = KEY_MINUS;
	/* XXX */
	else if (sym == XK_ISO_Level3_Shift)
		key = KEY_RIGHTALT;
	/* XXX */
	else
		key = 0;

	return key;
}

static int
vnc_pass_kbd_events(struct vnc_compositor *c, struct input_event_item *it)
{
	enum wl_keyboard_key_state state;
	uint32_t key, sym;

	if (it->down) {
		state = WL_KEYBOARD_KEY_STATE_PRESSED;
	} else {
		state = WL_KEYBOARD_KEY_STATE_RELEASED;
	}

	sym = it->keySym;
	key = vnc_keysym_to_key(sym);

	notify_key(&c->core_seat, it->time, key, state,
	    STATE_UPDATE_AUTOMATIC);

	return 1;
}

static int
vnc_input_handler(void *data)
{
	struct vnc_compositor *c = (struct vnc_compositor *)data;
	struct input_event_item *it, *next;
#if 0
	struct frame_finished_item *frameit;
#endif
	struct vnc_output *output = NULL;
	int changes = 0;

	pthread_mutex_lock(&c->input_mtx);
	if (wl_list_empty(&c->vnc_input_list)) {
		pthread_mutex_unlock(&c->input_mtx);
		return 0;
	}
	wl_list_for_each_reverse_safe(it, next, &c->vnc_input_list, link) {
		if (it->type == 1) {
			output = it->output;
			changes += vnc_pass_mouse_events(c, it);
		} else if (it->type == 2) {
			output = it->output;
			changes += vnc_pass_mouse_events(c, NULL);
			changes += vnc_pass_kbd_events(c, it);
		}
		free(it);
	}
	wl_list_init(&c->vnc_input_list);
	c->input_queue_length = 0;
	pthread_mutex_unlock(&c->input_mtx);

	changes += vnc_pass_mouse_events(c, NULL);

	if (output == NULL)
		return 1;

	/*
	 * XXX Introduce a separate timer source for explicit calling of
         *     finish_frame_handler
	 */
#if 0
	if (changes > 0) {
		frameit = zalloc(sizeof(struct frame_finished_item));
		if (frameit == NULL) {
			perror("zalloc");
			return 1;
		}
		clock_gettime(output->base.compositor->presentation_clock, &frameit->ts);
		pthread_mutex_lock(&c->frame_mtx);
		wl_list_insert(&c->vnc_frame_list, &frameit->link);
		pthread_mutex_unlock(&c->frame_mtx);
	}
	wl_event_source_activate(output->finish_frame_timer);
#endif

	rfbMarkRectAsModified(output->vncserver, 0, 0, 1, 1);

	return 1;
}

static int
vnc_input_create(struct vnc_compositor *c)
{
	weston_seat_init(&c->core_seat, &c->base, "default");

	weston_seat_init_pointer(&c->core_seat);

	if (weston_seat_init_keyboard(&c->core_seat, NULL) < 0) {
		fprintf(stderr, "%s: failed\n", __func__);
		return -1;
	}

	c->ptrx = 50;
	c->ptry = 50;
	c->ptrmask = 0;

	wl_list_init(&c->vnc_input_list);
	c->input_queue_length = 0;

	notify_motion_absolute(&c->core_seat, weston_compositor_get_time(), 50, 50);

	c->input_source = wl_event_loop_add_timer(
	    wl_display_get_event_loop(c->base.wl_display), vnc_input_handler, c);

	return 0;
}

static void
vnc_input_destroy(struct vnc_compositor *c)
{
	wl_event_source_remove(c->input_source);
	weston_seat_release(&c->core_seat);
}

static void
vnc_restore(struct weston_compositor *ec)
{
}

static void
vnc_destroy(struct weston_compositor *ec)
{
	struct vnc_compositor *c = (struct vnc_compositor *) ec;

	vnc_input_destroy(c);
	weston_compositor_shutdown(ec);

	free(ec);
}

static struct weston_compositor *
vnc_compositor_create(struct wl_display *display,
		      int width, int height, char *listen, int port,
		      const char *display_name, int *argc, char *argv[],
		      struct weston_config *config)
{
	struct vnc_compositor *c;

	c = zalloc(sizeof *c);
	if (c == NULL)
		return NULL;

	if (pthread_mutex_init(&c->input_mtx, NULL) != 0) {
		free(c);
		return NULL;
	}
	if (pthread_mutex_init(&c->finish_mtx, NULL) != 0) {
		free(c);
		return NULL;
	}
	if (pthread_mutex_init(&c->frame_mtx, NULL) != 0) {
		free(c);
		return NULL;
	}
	wl_list_init(&c->vnc_frame_list);

	if (weston_compositor_init(&c->base, display, argc, argv, config) < 0)
		goto err_free;

	if (weston_compositor_set_presentation_clock_software(&c->base) < 0)
		goto err_compositor;

	if (vnc_input_create(c) < 0)
		goto err_compositor;

	c->base.destroy = vnc_destroy;
	c->base.restore = vnc_restore;

	if (vnc_compositor_create_output(c, width, height, listen, port) < 0)
		goto err_input;

	if (pixman_renderer_init(&c->base) < 0)
		goto err_input;

	return &c->base;

err_input:
	vnc_input_destroy(c);
err_compositor:
	weston_compositor_shutdown(&c->base);
err_free:
	free(c);
	return NULL;
}

WL_EXPORT struct weston_compositor *
backend_init(struct wl_display *display, int *argc, char *argv[],
	     struct weston_config *config)
{
	int width = 1024, height = 640;
	char *display_name = NULL;
	char *listen;
	int port;

	const struct weston_option vnc_options[] = {
		{ WESTON_OPTION_INTEGER, "width", 0, &width },
		{ WESTON_OPTION_INTEGER, "height", 0, &height },
		{ WESTON_OPTION_STRING, "vnclisten", 0, &listen },
		{ WESTON_OPTION_INTEGER, "vncport", 0, &port },
	};

	parse_options(vnc_options,
		      ARRAY_LENGTH(vnc_options), argc, argv);

	return vnc_compositor_create(display, width, height, listen, port,
				     display_name, argc, argv, config);
}
