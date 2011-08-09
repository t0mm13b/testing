#include <stdlib.h>
#include <stdio.h>
#include <errno.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <sys/poll.h>
#include <limits.h>
#include <linux/input.h>

#define MAX_DEVICES		2

static struct pollfd ev_fds[MAX_DEVICES];
static struct ev evs[MAX_DEVICES];
static unsigned ev_count = 0;

static int fd_touchscreen = 0;
static int fd_keys = 0;

void cleanup(void);

int main(int argc, char **argv){
	const char *ts_event = "/dev/input/event3";
	const char *key_event = "/dev/input/event0";
	int nloop = 0;
	//
	atexit(cleanup);
	//
	fd_touchscreen = open(ts_event, O_RDONLY);
	if (fd_touchscreen < 0){
		printf("Error! Could not open %s for reading\n", ts_event);
		exit(-1);
	}
	printf("Opened %s successfully\n", ts_event);
	fd_keys = open(key_event, O_RDONLY);
	if (fd_keys < 0){
		printf("Error! Could not open %s for reading\n", key_event);
		exit(-2);
	}
	// Set up the polling descriptors 0 = touchscreen, 1 = hardware keys
	ev_fds[0].fd = fd_touchscreen;
	ev_fds[0].events = POLLIN;
	//
	ev_fds[1].fd = fd_keys;
	ev_fds[1].events = POLLIN;
	//
	//
	return 0;
}
void cleanup(void){
	if (fd_touchscreen > 0) close(fd_touchscreen);
	if (fd_keys > 0) close(fd_keys);
}

int read_inputs(struct input_event *ev, unsigned dont_wait)
{
    int r;
    unsigned n;

    do {
        r = poll(ev_fds, MAX_DEVICES, dont_wait ? 0 : -1);

        if(r > 0) {
            for(n = 0; n < ev_count; n++) {
                if(ev_fds[n].revents & POLLIN) {
                    r = read(ev_fds[n].fd, ev, sizeof(*ev));
                    if(r == sizeof(*ev)) {
                        
                            return 0;
                    }
                }
            }
        }
    } while(dont_wait == 0);

    return -1;
}

static int vk_inside_display(__s32 value, struct input_absinfo *info, int screen_size)
{
    int screen_pos;

    if (info->minimum == info->maximum)
        return 0;

    screen_pos = (value - info->minimum) * (screen_size - 1) / (info->maximum - info->minimum);
    return (screen_pos >= 0 && screen_pos < screen_size);
}

static int vk_tp_to_screen(struct position *p, int *x, int *y)
{
    if (p->xi.minimum == p->xi.maximum || p->yi.minimum == p->yi.maximum)
        return 0;

    *x = (p->x - p->xi.minimum) * (gr_fb_width() - 1) / (p->xi.maximum - p->xi.minimum);
    *y = (p->y - p->yi.minimum) * (gr_fb_height() - 1) / (p->yi.maximum - p->yi.minimum);

    if (*x >= 0 && *x < gr_fb_width() &&
           *y >= 0 && *y < gr_fb_height()) {
        return 0;
    }

    return 1;
}

/* Translate a virtual key in to a real key event, if needed */
/* Returns non-zero when the event should be consumed */
static int vk_modify(struct ev *e, struct input_event *ev)
{
    int i;
    int x, y;

    if (ev->type == EV_KEY) {
        if (ev->code == BTN_TOUCH)
            e->p.pressed = ev->value;
        return 0;
    }

    if (ev->type == EV_ABS) {
        switch (ev->code) {
        case ABS_X:
            e->p.x = ev->value;
            return !vk_inside_display(e->p.x, &e->p.xi, gr_fb_width());
        case ABS_Y:
            e->p.y = ev->value;
            return !vk_inside_display(e->p.y, &e->p.yi, gr_fb_height());
        case ABS_MT_POSITION_X:
            if (e->mt_idx) return 1;
            e->mt_p.x = ev->value;
            return !vk_inside_display(e->mt_p.x, &e->mt_p.xi, gr_fb_width());
        case ABS_MT_POSITION_Y:
            if (e->mt_idx) return 1;
            e->mt_p.y = ev->value;
            return !vk_inside_display(e->mt_p.y, &e->mt_p.yi, gr_fb_height());
        case ABS_MT_TOUCH_MAJOR:
            if (e->mt_idx) return 1;
            if (e->sent)
                e->mt_p.pressed = (ev->value > 0);
            else
                e->mt_p.pressed = (ev->value > PRESS_THRESHHOLD);
            return 0;
        }

        return 0;
    }

    if (ev->type != EV_SYN)
        return 0;

    if (ev->code == SYN_MT_REPORT) {
        /* Ignore the rest of the points */
        ++e->mt_idx;
        return 1;
    }
    if (ev->code != SYN_REPORT)
        return 0;

    /* Report complete */

    e->mt_idx = 0;

    if (!e->p.pressed && !e->mt_p.pressed) {
        /* No touch */
        e->sent = 0;
        return 0;
    }

    if (!(e->p.pressed && vk_tp_to_screen(&e->p, &x, &y)) &&
            !(e->mt_p.pressed && vk_tp_to_screen(&e->mt_p, &x, &y))) {
        /* No touch inside vk area */
        return 0;
    }

    if (e->sent) {
        /* We've already sent a fake key for this touch */
        return 1;
    }

    /* The screen is being touched on the vk area */
    e->sent = 1;

    for (i = 0; i < e->vk_count; ++i) {
        int xd = ABS(e->vks[i].centerx - x);
        int yd = ABS(e->vks[i].centery - y);
        if (xd < e->vks[i].width/2 && yd < e->vks[i].height/2) {
            /* Fake a key event */
            ev->type = EV_KEY;
            ev->code = e->vks[i].scancode;
            ev->value = 1;

            vibrate(VIBRATOR_TIME_MS);
            return 0;
        }
    }

    return 1;
}

int ev_get(struct input_event *ev, unsigned dont_wait)
{
    int r;
    unsigned n;

    do {
        r = poll(ev_fds, ev_count, dont_wait ? 0 : -1);

        if(r > 0) {
            for(n = 0; n < ev_count; n++) {
                if(ev_fds[n].revents & POLLIN) {
                    r = read(ev_fds[n].fd, ev, sizeof(*ev));
                    if(r == sizeof(*ev)) {
                        if (!vk_modify(&evs[n], ev))
                            return 0;
                    }
                }
            }
        }
    } while(dont_wait == 0);

    return -1;
}
