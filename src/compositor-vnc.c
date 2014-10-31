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

struct input_event_item {
	int type;
	rfbBool down;
	rfbKeySym keySym;
	uint32_t time;
	int buttonMask;
	int xabs;
	int yabs;
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
};

struct vnc_output {
	struct weston_output base;
	struct weston_mode mode;
	struct wl_event_source *finish_frame_timer;
	rfbScreenInfoPtr vncserver;
	pixman_image_t *shadow_surface;
	struct vnc_compositor *c;
	pthread_cond_t repaint_cond;
};


static void
vnc_output_start_repaint_loop(struct weston_output *output)
{
#if 0
	struct timespec ts;

	clock_gettime(output->compositor->presentation_clock, &ts);
	weston_output_finish_frame(output, &ts);
#endif
}

static int
finish_frame_handler(void *data)
{
	struct weston_output *output = (struct weston_output *)data;
	struct vnc_output *vncoutput = (struct vnc_output *)data;
	struct timespec ts;

//	fprintf(stderr, "%s: called\n", __func__);

	clock_gettime(output->compositor->presentation_clock, &ts);
	weston_output_finish_frame(output, &ts);
	wl_event_source_timer_update(vncoutput->finish_frame_timer, 40);

//	fprintf(stderr, "%s: done\n", __func__);

//	vnc_output_start_repaint_loop(data);

	pthread_mutex_lock(&vncoutput->c->finish_mtx);
	pthread_cond_signal(&vncoutput->repaint_cond);
	pthread_mutex_unlock(&vncoutput->c->finish_mtx);

	return 1;
}

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
	pixman_renderer_output_set_buffer(base, output->shadow_surface);
	pthread_mutex_lock(&output->c->finish_mtx);
	ec->renderer->repaint_output(base, damage);
	pthread_mutex_unlock(&output->c->finish_mtx);

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

	wl_event_source_remove(output->finish_frame_timer);
	free(output);

	return;
}

static void
vnc_display_event(struct _rfbClientRec *cl)
{
	struct vnc_output *output = cl->screen->screenData;

//	fprintf(stderr, "%s: called\n", __func__);

	pthread_mutex_lock(&output->c->finish_mtx);
	wl_event_source_activate(output->finish_frame_timer);
	pthread_cond_wait(&output->repaint_cond, &output->c->finish_mtx);
	pthread_mutex_unlock(&output->c->finish_mtx);

	/* XXX */
}

static void
vnc_displayfinished_event(struct _rfbClientRec *cl, int result)
{
//	struct vnc_output *output = cl->screen->screenData;

//	fprintf(stderr, "%s: called\n", __func__);
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

	if (pthread_cond_init(&output->repaint_cond, NULL) != 0) {
		free(output);
		return -1;
	}

	output->c = c;
	output->vncserver = rfbGetScreen(NULL, NULL, width, height, 8, 3, 4);
	output->vncserver->deferUpdateTime = 30;
	output->vncserver->screenData = output;
	output->vncserver->frameBuffer = calloc(width * height, 4);
	/* XXX clean up on calloc failure */
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

	output->shadow_surface =
		pixman_image_create_bits(PIXMAN_x8b8g8r8,
					 width, height,
					 (uint32_t *)output->vncserver->frameBuffer,
					 width * 4);
	if (output->shadow_surface == NULL) {
		weston_log("Failed to create surface for frame buffer.\n");
		/* XXX clean up correctly */
		return -1;
	}

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
	output->base.assign_planes = NULL;
	output->base.set_backlight = NULL;
	output->base.set_dpms = NULL;
	output->base.switch_mode = NULL;

	wl_list_insert(c->base.output_list.prev, &output->base.link);

	rfbInitServer(output->vncserver);
	rfbRunEventLoop(output->vncserver, -1, 1);

	return 0;
}

static void
vnc_pass_mouse_events(struct vnc_compositor *c, struct input_event_item *it)
{
	wl_fixed_t wl_x, wl_y;
	int nm;

	static uint32_t lasttime;
	static int prevx = 0, prevy = 0;
	static int prevmask = 0;
	static int lazymotion = 0;

	if (it == NULL && lazymotion) {
		wl_x = wl_fixed_from_int(prevx);
		wl_y = wl_fixed_from_int(prevy);
		notify_motion_absolute(&c->core_seat, lasttime, wl_x, wl_y);
		lazymotion = 0;
	}
	if (it == NULL)
		return;

	nm = it->buttonMask;
	lasttime = it->time;

	if (prevx != it->xabs || prevy != it->yabs) {
//		fprintf(stderr, "moving from %dx%d to %dx%d\n", prevx, prevy, it->xabs, it->yabs);
		lazymotion = 1;
	}
	prevx = it->xabs;
	prevy = it->yabs;
	if (prevmask != nm) {
		wl_x = wl_fixed_from_int(prevx);
		wl_y = wl_fixed_from_int(prevy);
		notify_motion_absolute(&c->core_seat, it->time, wl_x, wl_y);
		lazymotion = 0;
//		fprintf(stderr, "setting mask from %d to %d\n", prevmask, nm);
		if ((prevmask & 1) != (nm & 1)) {
			notify_button(&c->core_seat,
			    it->time, BTN_LEFT,
			    (nm & 1) ? WL_POINTER_BUTTON_STATE_PRESSED :
			               WL_POINTER_BUTTON_STATE_RELEASED);
		}
		if ((prevmask & 2) != (nm & 2)) {
			notify_button(&c->core_seat,
			    it->time, BTN_MIDDLE,
			    (nm & 2) ? WL_POINTER_BUTTON_STATE_PRESSED :
			               WL_POINTER_BUTTON_STATE_RELEASED);
		}
		if ((prevmask & 4) != (nm & 4)) {
			notify_button(&c->core_seat,
			    it->time, BTN_RIGHT,
			    (nm & 4) ? WL_POINTER_BUTTON_STATE_PRESSED :
			               WL_POINTER_BUTTON_STATE_RELEASED);
		}
		if ((prevmask & 8) != (nm & 8)) {
			notify_axis(&c->core_seat,
			    it->time, WL_POINTER_AXIS_VERTICAL_SCROLL,
			    -DEFAULT_AXIS_STEP_DISTANCE);
		}
		if ((prevmask & 16) != (nm & 16)) {
			notify_axis(&c->core_seat,
			    it->time, WL_POINTER_AXIS_VERTICAL_SCROLL,
			    DEFAULT_AXIS_STEP_DISTANCE);
		}
		prevmask = nm;
	}
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

static void
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
}

static int
vnc_input_handler(void *data)
{
	struct vnc_compositor *c = (struct vnc_compositor *)data;
	struct input_event_item *it, *next;

	pthread_mutex_lock(&c->input_mtx);
	if (wl_list_empty(&c->vnc_input_list)) {
		pthread_mutex_unlock(&c->input_mtx);
		return 0;
	}
	wl_list_for_each_reverse_safe(it, next, &c->vnc_input_list, link) {
		/* XXX handle mouse and keyboard events */
		if (it->type == 1) {
			vnc_pass_mouse_events(c, it);
		} else if (it->type == 2) {
			vnc_pass_mouse_events(c, NULL);
			vnc_pass_kbd_events(c, it);
		}
		free(it);
	}
	wl_list_init(&c->vnc_input_list);
	c->input_queue_length = 0;
	pthread_mutex_unlock(&c->input_mtx);

	vnc_pass_mouse_events(c, NULL);

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
