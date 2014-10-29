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
#include <sys/time.h>
#include <linux/input.h>

#include <rfb/rfb.h>

#include "compositor.h"
#include "pixman-renderer.h"

struct vnc_compositor {
	struct weston_compositor base;
	struct weston_seat core_seat;
	struct wl_event_source *input_source;
	int ptrx, ptry, ptrmask;
};

struct vnc_output {
	struct weston_output base;
	struct weston_mode mode;
	struct wl_event_source *finish_frame_timer;
	rfbScreenInfoPtr vncserver;
	pixman_image_t *shadow_surface;
};


static void
vnc_output_start_repaint_loop(struct weston_output *output)
{
	struct timespec ts;

	clock_gettime(output->compositor->presentation_clock, &ts);
	weston_output_finish_frame(output, &ts);
}

static int
finish_frame_handler(void *data)
{
	vnc_output_start_repaint_loop(data);

	return 1;
}

static int
vnc_output_repaint(struct weston_output *base,
		   pixman_region32_t *damage)
{
	struct vnc_output *output = (struct vnc_output *) base;
	struct weston_compositor *ec = output->base.compositor;
	pixman_box32_t *rects;
	int nrects, i, x1, y1, x2, y2, width, height;

	/* Repaint the damaged region onto the back buffer. */
	pixman_renderer_output_set_buffer(base, output->shadow_surface);
	ec->renderer->repaint_output(base, damage);

	/* Transform and composite onto the frame buffer. */
	width = pixman_image_get_width(output->shadow_surface);
	height = pixman_image_get_height(output->shadow_surface);
	rects = pixman_region32_rectangles(damage, &nrects);

	for (i = 0; i < nrects; i++) {
		x1 = rects[i].x1;
		x2 = rects[i].x2;
		y1 = rects[i].y1;
		y2 = rects[i].y2;
		rfbMarkRectAsModified(output->vncserver, x1, y1, x2, y2);
#if 0
		switch (base->transform) {
		default:
		case WL_OUTPUT_TRANSFORM_NORMAL:
			x1 = rects[i].x1;
			x2 = rects[i].x2;
			y1 = rects[i].y1;
			y2 = rects[i].y2;
			break;
		case WL_OUTPUT_TRANSFORM_180:
			x1 = width - rects[i].x2;
			x2 = width - rects[i].x1;
			y1 = height - rects[i].y2;
			y2 = height - rects[i].y1;
			break;
		case WL_OUTPUT_TRANSFORM_90:
			x1 = height - rects[i].y2;
			x2 = height - rects[i].y1;
			y1 = rects[i].x1;
			y2 = rects[i].x2;
			break;
		case WL_OUTPUT_TRANSFORM_270:
			x1 = rects[i].y1;
			x2 = rects[i].y2;
			y1 = width - rects[i].x2;
			y2 = width - rects[i].x1;
			break;
		}
		src_x = x1;
		src_y = y1;

		pixman_image_composite32(PIXMAN_OP_SRC,
			output->shadow_surface, /* src */
			NULL, /* mask */
			output->hw_surface, /* dest */
			src_x, src_y, /* src_x, src_y */
			0, 0, /* mask_x, mask_y */
			x1, y1, /* dest_x, dest_y */
			x2 - x1, /* width */
			y2 - y1 /* height */);
#endif
	}
	fprintf(stderr, "%s: repainted\n", __func__);

	pixman_region32_subtract(&ec->primary_plane.damage,
				 &ec->primary_plane.damage, damage);

	wl_event_source_timer_update(output->finish_frame_timer, 16);

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
vnc_ptr_event(int buttonMask, int x, int y, struct _rfbClientRec *cl)
{
	struct vnc_compositor *c = cl->screen->screenData;

	fprintf(stderr, "%s: buttonMask=%d, x=%d, y=%d\n", __func__,
	    buttonMask, x, y);

//	notify_motion_absolute(&c->core_seat, weston_compositor_get_time(), x*100, y*100);
	c->ptrx = x;
	c->ptry = y;
	c->ptrmask = buttonMask;
	wl_event_source_timer_update(c->input_source, 1000);
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

	output->vncserver = rfbGetScreen(NULL, NULL, width, height, 8, 3, 4);
	output->vncserver->screenData = c;
	output->vncserver->frameBuffer = calloc(width * height, 4);
	/* XXX clean up on calloc failure */
	output->vncserver->ptrAddEvent = vnc_ptr_event;
//	output->vncserver->kbdAddEvent = vnc_kbd_event;
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
	output->mode.refresh = 60;
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

static int
vnc_input_handler(void *data)
{
	struct vnc_compositor *c = (struct vnc_compositor *)data;
	wl_fixed_t wl_x, wl_y;
static int prevx = 0, prevy = 0;

	wl_x = wl_fixed_from_int((int)c->ptrx);
	wl_y = wl_fixed_from_int((int)c->ptry);
	if (prevx != c->ptrx || prevy != c->ptry) {
		fprintf(stderr, "moving from %dx%d to %dx%d\n", prevx, prevy, c->ptrx, c->ptry);
		notify_motion_absolute(&c->core_seat, weston_compositor_get_time(), wl_x, wl_y);
	}
	prevx = c->ptrx;
	prevy = c->ptry;
static int prevmask = 0;
	if (prevmask != c->ptrmask) {
		fprintf(stderr, "setting mask from %d to %d\n", prevmask, c->ptrmask);
		if ((prevmask & 1) != (c->ptrmask & 1)) {
			notify_button(&c->core_seat,
			    weston_compositor_get_time(), BTN_LEFT,
			    (c->ptrmask & 1) ? WL_POINTER_BUTTON_STATE_PRESSED :
			                       WL_POINTER_BUTTON_STATE_RELEASED);
		}
		if ((prevmask & 2) != (c->ptrmask & 2)) {
			notify_button(&c->core_seat,
			    weston_compositor_get_time(), BTN_MIDDLE,
			    (c->ptrmask & 2) ? WL_POINTER_BUTTON_STATE_PRESSED :
			                       WL_POINTER_BUTTON_STATE_RELEASED);
		}
		if ((prevmask & 4) != (c->ptrmask & 4)) {
			notify_button(&c->core_seat,
			    weston_compositor_get_time(), BTN_RIGHT,
			    (c->ptrmask & 4) ? WL_POINTER_BUTTON_STATE_PRESSED :
			                       WL_POINTER_BUTTON_STATE_RELEASED);
		}
		prevmask = c->ptrmask;
	}

	wl_event_source_timer_update(c->input_source, 1000);

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
	weston_seat_init_pointer(&c->core_seat);

	c->ptrx = 50;
	c->ptry = 50;
	c->ptrmask = 0;

	notify_motion_absolute(&c->core_seat, weston_compositor_get_time(), 50, 50);

	c->input_source = wl_event_loop_add_timer(
	    wl_display_get_event_loop(c->base.wl_display), vnc_input_handler, c);
	wl_event_source_timer_update(c->input_source, 1000);

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
