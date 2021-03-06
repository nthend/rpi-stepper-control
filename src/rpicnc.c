#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include <unistd.h>
#include <signal.h>
#include <pthread.h>

#include <pigpio.h>

#include "command.h"
#include "generator.h"
#include "axis.h"
#include "axis_task.h"
#include "device.h"
#include "task.h"


#include "ringbuffer.h"
typedef Task* TaskPtr;
ringbuffer_declare(TaskQueue, tq, TaskPtr)
ringbuffer_define(TaskQueue, tq, TaskPtr)

#include "rpicnc.h"

#define DEBUG


static int initialized = 0;

static Device device;
static Generator generator;

static pthread_t thread;
static pthread_mutex_t mutex;
static uint8_t thread_done = 1;

static TaskQueue *task_queue;
#define TQLEN 0x100


int cnc_init(int axes_count, AxisInfo *axes_info) {
	printf("[ cnc ] %s\n", __func__);

	if (initialized) {
		printf("[error] cnc already initialized\n");
		return 1;
	}

	if (gpioInitialise() < 0) {
		printf("[error] cannot initialize cnc\n");
		return 2;
	}

	gen_init(&generator, 0x10);
	dev_init(&device, axes_count);

	task_queue = tq_init(TQLEN);
	pthread_mutex_init(&mutex, NULL);

#ifdef DEBUG
	printf("axes_count: %d\n", axes_count);
#endif /* DEBUG */

	int i;
	for (i = 0; i < axes_count; ++i) {
		AxisInfo *ai = &axes_info[i];
		axis_init(
			&device.axes[i],
			ai->mask_step_pos, ai->mask_step_neg,
			ai->mask_dir_pos, ai->mask_dir_neg,
			ai->sense, ai->pin_left, ai->pin_right
		);
		device.axes[i].position = ai->position;
		device.axes[i].length = ai->length;
#ifdef DEBUG
		printf("axis[%d]:\n", i);
		printf("\tstep: (%08x, %08x),\n", ai->mask_step_pos, ai->mask_step_neg);
		printf("\tdir:  (%08x, %08x),\n", ai->mask_dir_pos, ai->mask_dir_neg);
		printf("\tsense: %d, left: %d, right: %d\n", ai->sense, ai->pin_left, ai->pin_right);
#endif /* DEBUG */
	}

	initialized = 1;

	return 0;
}

int cnc_quit() {
	printf("[ cnc ] %s\n", __func__);
	if (!initialized) {
		printf("[error] cnc was not initialized\n");
		return 1;
	}

	cnc_stop();

	int i;
	for (i = 0; i < device.axis_count; ++i) {
		axis_free(&device.axes[i]);
	}

	tq_free(task_queue);
	pthread_mutex_destroy(&mutex);

	dev_free(&device);
	gen_free(&generator);

	gpioTerminate();

	initialized = 0;
	return 0;
}

int cnc_clear() {
	printf("[ cnc ] %s\n", __func__);
	gen_clear(&generator);
	dev_clear(&device);
	return 0;
}

typedef struct {
	Cmd *cmds;
	int pos;
	int len;
} _CmdsChannel;

typedef struct {
	_CmdsChannel chs[MAX_AXES];
} _CmdsCookie;

Cmd _next_cmd(int axis, void *userdata) {
	_CmdsCookie *cookie = (_CmdsCookie*) userdata;
	Cmd cmd = cmd_idle();
	_CmdsChannel *ch = &cookie->chs[axis];
	if (ch->pos < ch->len) {
		cmd = ch->cmds[ch->pos];
		ch->pos += 1;
	}
	return cmd;
}

int cnc_run_task(Task *task) {
	printf("[ cnc ] %s\n", __func__);
	if (task->type == TASK_NONE) {
		// pass
	} else if (task->type == TASK_SCAN) {
		if (task->scan.axis < 0 || task->scan.axis >= device.axis_count) {
			return 2;
		}
		Axis *axis = &device.axes[task->scan.axis];
		axis->length = 0;
		axis_scan(axis, &generator, task->scan.vel_ini, task->scan.vel_max, task->scan.acc_max);
		printf("length: %d\n", axis->length);
		task->scan.length = axis->length;
	} else if (task->type == TASK_CALIB) {
		if (task->scan.axis < 0 || task->scan.axis >= device.axis_count) {
			return 2;
		}
		Axis *axis = &device.axes[task->calib.axis];
		axis_calib(axis, &generator, &task->calib.vel_ini, &task->calib.vel_max, &task->calib.acc_max);
	} else if (task->type == TASK_CMDS) {
		_CmdsCookie cookie;
		int i, edge = 0;
		int axpos[MAX_AXES];
		for (i = 0; i < device.axis_count; ++i) {
			cookie.chs[i].cmds = task->cmds.cmds[i];
			cookie.chs[i].len = task->cmds.cmds_count[i];
			cookie.chs[i].pos = 0;
			
			Axis *axis = &device.axes[i];
			axpos[i] = 0;
			int first = 1;
			int l = 0, r = 0;
			if (axis->sense) {
				l = gpioRead(axis->pin_left);
				r = gpioRead(axis->pin_right);
			}
			Cmd *cmds = task->cmds.cmds[i];
			int j;
			int len = task->cmds.cmds_count[i];
			for (j = 0; j < len; ++j) {
				if (cmds[j].type == CMD_MOVE) {
					int dir = cmds[j].move.dir;
					if (axis->sense && first && ((dir && r) || (!dir && l))) {
						edge = 1;
						printf("[error] out of bounds at axis:%d cmd:%d\n", i, j);
					}
					first = 0;
					axpos[i] += cmds[j].move.steps*(dir ? 1 : -1);
				}
			}
		}
		if (edge) {
			// axis will move out of bounds
			return 2;
		}

		dev_run(&device, &generator, _next_cmd, (void*) &cookie);
		cnc_clear();

		for (i = 0; i < device.axis_count; ++i) {
			Axis *axis = &device.axes[i];
			axis->position += axpos[i];
			if (gpioRead(axis->pin_right)) {
				axis->position = axis->length;
			}
			if (gpioRead(axis->pin_left)) {
				axis->position = 0;
			}
		}
	} else {
		// unknown task type
		return 1;
	}
	gpioDelay(10000); //us
	return 0;
}

int cnc_read_sensors() {
	//printf("[ cnc ] %s\n", __func__);
	int res = 0;
	int i;
	for (i = 0; i < device.axis_count; ++i) {
		res |= (axis_read_sensors(&device.axes[i])<<(2*i));
	}
	return res;
}

int cnc_axes_info(AxisInfo *axes_info) {
	int i;
	for (i = 0; i < device.axis_count; ++i) {
		AxisInfo *ai = &axes_info[i];
		Axis *ax = &device.axes[i];

		ai->mask_step_pos  = ax->mask_step_pos;
		ai->mask_step_neg  = ax->mask_step_neg;
		ai->mask_dir_pos   = ax->mask_dir_pos;
		ai->mask_dir_neg   = ax->mask_dir_neg;
		ai->sense  = ax->sense;
		ai->pin_left  = ax->pin_left;
		ai->pin_right = ax->pin_right;

		ai->position  = ax->position;
		ai->length    = ax->length;
	}
	return 0;
}


// asynchronous

int cnc_push_task(Task *task) {
	printf("[ cnc ] %s\n", __func__);

	pthread_mutex_lock(&mutex); /// <- LOCK MUTEX
	if (tq_full(task_queue)) {
		pthread_mutex_unlock(&mutex); /// <- UNLOCK MUTEX
		printf("[error] task_queue full\n");
		return 1;
	}
	tq_push(task_queue, &task);
	pthread_mutex_unlock(&mutex); /// <- UNLOCK MUTEX

	return 0;
}

void *_thread_main(void *cookie) {
	while (!thread_done) {
		Task *task;
		pthread_mutex_lock(&mutex); /// <- LOCK MUTEX
		if(tq_empty(task_queue)) {
			pthread_mutex_unlock(&mutex); /// <- UNLOCK MUTEX
			break;
		}
		tq_pop(task_queue, &task);
		pthread_mutex_unlock(&mutex); /// <- UNLOCK MUTEX
		cnc_run_task(task);
	}

	thread_done = 1;
	return NULL;
}

int cnc_run_async() {
	printf("[ cnc ] %s\n", __func__);

	if (!thread_done) {
		return 0;
	}

	thread_done = 0;
	int s = pthread_create(&thread, NULL, _thread_main, NULL);
	if (s) {
		thread_done = 1;
		perror("pthread_create error");
		return 1;
	}

	return 0;
}

int cnc_is_busy() {
	//printf("[ cnc ] %s\n", __func__);

	int occ = 0;
	pthread_mutex_lock(&mutex); /// <- LOCK MUTEX
	occ = tq_occupancy(task_queue) + !thread_done;
	pthread_mutex_unlock(&mutex); /// <- UNLOCK MUTEX
	return occ;
}

int cnc_wait() {
	printf("[ cnc ] %s\n", __func__);

	if (!thread_done) {
		pthread_join(thread, NULL);
	}
	return 0;
}

int cnc_stop() {
	printf("[ cnc ] %s\n", __func__);

	gen_stop(&generator);
	pthread_cancel(thread);
	thread_done = 1;
	cnc_clear();

	while(!tq_empty(task_queue)) {
		tq_pop(task_queue, NULL);
	}

	return 0;
}
