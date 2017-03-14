#pragma once

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <signal.h>

#include <pigpio.h>

#define MODE_NONE 0x00
#define MODE_SCAN 0x11
#define MODE_MOVE 0x21

#define STAGE_NONE 0x00 
#define STAGE_SCAN_LR 0x11
#define STAGE_SCAN_RL 0x12

typedef struct Axis {
	int step_pin;
	int dir_pin;
	int left_pin;
	int right_pin;
	int length;
	int position;
	int ramp;
	
	int _mode;
	int _dir;
	int _stage;
	uint32_t _last_tick;
	uint32_t _broken;
	uint32_t _ticks;
	int _delay;
	int _wave_id;
} Axis;

void _stop_all() {
	gpioWaveTxStop();
	gpioWaveClear();
}

int _wave_gen_step(int pin, int delay) {
	gpioPulse_t pulses[2];
	pulses[0].usDelay = delay/2;
	pulses[0].gpioOn = 1<<pin;
	pulses[0].gpioOff = 0;
	pulses[1].usDelay = (delay - 1)/2 + 1;
	pulses[1].gpioOn = 0;
	pulses[1].gpioOff = 1<<pin;
	
	gpioWaveAddNew();

	gpioWaveAddGeneric(2, pulses);
	return gpioWaveCreate();
}

int _wave_gen_ramp(int pin, int begin_delay, int end_delay, int steps) {
	int i;
	gpioPulse_t *pulses = (gpioPulse_t *) malloc(sizeof(gpioPulse_t)*2*steps);
	
	for (i = 0; i < 2*steps; ++i) {
		float bf = 1.0/begin_delay;
		float ef = 1.0/end_delay;
		float f = bf + (ef - bf)*(0.5*i + 0.25)/steps;
		int delay = 1.0/f;
		// delay = delay < begin_delay ? begin_delay : (delay > end_delay ? end_delay : delay);
		pulses[i].usDelay = delay/2;
		if (i % 2 == 0) {
			pulses[i].gpioOn = 1<<pin;
			pulses[i].gpioOff = 0;
		} else {
			pulses[i].gpioOn = 0;
			pulses[i].gpioOff = 1<<pin;
		}
	}
	
	gpioWaveAddNew();

	gpioWaveAddGeneric(2*steps, pulses);
	return gpioWaveCreate();
}

int _set_dir_safe(Axis *axis, int dir) {
	if (dir == 0) {
		if (!gpioRead(axis->left_pin)) {
			gpioWrite(axis->dir_pin, 0);
			return 0;
		} else {
			return -1;
		}
	} else {
		if (!gpioRead(axis->right_pin)) {
			gpioWrite(axis->dir_pin, 1);
			return 0;
		} else {
			return -1;
		}
	}
}

int _wave_wait() {
	while (gpioWaveTxBusy()) {
		gpioDelay(10000);
	}
	return 0;
}

int _print_wave_info(int wave_id) {
	rawWaveInfo_t info = rawWaveInfo(wave_id);
	printf("rawWaveInfo: {\n");
	printf("botCB: %d\n", info.botCB);
	printf("topCB: %d\n", info.topCB);
	printf("botOOL: %d\n", info.botOOL);
	printf("topOOL: %d\n", info.topOOL);
	printf("deleted: %d\n", info.deleted);
	printf("numCB: %d\n", info.numCB);
	printf("numBOOL: %d\n", info.numBOOL);
	printf("numTOOL: %d\n", info.numTOOL);
	printf("}\n");
	return 0;
}

void _axis_alert_cb(int gpio, int level, uint32_t tick, void *userdata) {
	Axis *axis = (Axis*) userdata;
	if (axis != NULL) {
		if(axis->left_pin == gpio && level) {
			printf("left\n");
			gpioWaveTxStop();
		} else if(axis->right_pin == gpio && level) {
			printf("right\n");
			gpioWaveTxStop();
		}
		if (axis->_mode == MODE_SCAN) {
			if(axis->left_pin == gpio && level) {
				if(axis->_stage == STAGE_SCAN_RL) {
					axis->_ticks = tick - axis->_last_tick;
				}
			} else if(axis->right_pin == gpio && level) {
				if(axis->_stage == STAGE_SCAN_LR) {
					axis->_ticks = tick - axis->_last_tick;
				}
			}
		} else if (axis->_mode == MODE_MOVE) {
			if(axis->left_pin == gpio && level) {
				axis->_broken = 1;
				axis->_ticks = tick - axis->_last_tick;
			} else if(axis->right_pin == gpio && level) {
				axis->_broken = 1;
				axis->_ticks = tick - axis->_last_tick;
			}
		} else {
			_stop_all();
		}
	} else {
		_stop_all();
	}
}

int axis_init(Axis *axis, int step, int dir, int left, int right) {
	axis->step_pin = step;
	axis->dir_pin = dir;
	axis->left_pin = left;
	axis->right_pin = right;
	
	axis->position = 0;
	axis->length = 0;
	axis->ramp = 0;
	
	axis->_mode = MODE_NONE;
	axis->_stage = STAGE_NONE;

	gpioSetMode(axis->step_pin, PI_OUTPUT);
	gpioSetMode(axis->dir_pin,  PI_OUTPUT);
	gpioSetMode(axis->left_pin,  PI_INPUT);
	gpioSetMode(axis->right_pin, PI_INPUT);
	
	gpioSetAlertFuncEx(axis->left_pin, _axis_alert_cb, (void*) axis);
	gpioSetAlertFuncEx(axis->right_pin, _axis_alert_cb, (void*) axis);
	
	return 0;
}

int _axis_scan(Axis *axis, int dir) {
	if (_set_dir_safe(axis, dir) != 0) {
		return 0;
	}
	
	axis->_wave_id = _wave_gen_step(axis->step_pin, axis->_delay);
	if (axis->_wave_id < 0) {
		printf("error create wave\n");
		return -1;
	}
	
	axis->_last_tick = gpioTick();
	gpioWaveTxSend(axis->_wave_id, PI_WAVE_MODE_REPEAT);
	_wave_wait(); // sleep until done
	_print_wave_info(axis->_wave_id);
	
	gpioWaveDelete(axis->_wave_id);
	axis->_wave_id = -1;
	
	return axis->_ticks/axis->_delay;
}

int axis_scan(Axis *axis) {
	axis->_mode = MODE_SCAN;
	axis->_delay = 1000;
	
	axis->_stage = STAGE_SCAN_RL;
	if (_axis_scan(axis, 0) < 0) {
		return -1;
	}
	
	gpioDelay(10000);
	axis->_stage = STAGE_SCAN_LR;
	int lr = _axis_scan(axis, 1);
	if (lr < 0) {
		return -1;
	}
	printf("lr steps: %d\n", lr);
	
	gpioDelay(10000);
	axis->_stage = STAGE_SCAN_RL;
	int rl = _axis_scan(axis, 0);
	if (rl < 0) {
		return -1;
	}
	printf("rl steps: %d\n", rl);
	
	axis->_mode = MODE_NONE;
	
	int min_len = rl < lr ? rl : lr;
	axis->length = min_len;
	axis->position = 0;
	
	return min_len;
}

int _axis_move_rel(Axis *axis, int steps, int dir) {
	axis->_mode = MODE_MOVE;
	axis->_delay = 1000;
	
	int wave_begin, wave_step, wave_end;
	wave_begin = _wave_gen_ramp(axis->step_pin, 1000000, axis->_delay, axis->ramp);
	wave_step = _wave_gen_step(axis->step_pin, axis->_delay);
	wave_end = _wave_gen_ramp(axis->step_pin, axis->_delay, 1000000, axis->ramp);
	if (wave_begin < 0 || wave_step < 0 || wave_end < 0) {
		fprintf(stderr, "error create wave\n");
	}

	char comp[4] = {
		0xff & (steps >>  0), 0xff & (steps >>  8), 
		0xff & (steps >> 16), 0xff & (steps >> 24)
	};
	char chain[] = {
		wave_begin,
		255, 0,
			255, 0,
				wave_step,
			255, 1, 255, 255,
		255, 1, comp[2], comp[3],
		255, 0,
			wave_step,
		255, 1, comp[0], comp[1],
		wave_end
	};
	
	if (_set_dir_safe(axis, dir) != 0) {
		return 0;
	}
	
	axis->_broken = 0;
	axis->_last_tick = gpioTick();
	int cs = gpioWaveChain(chain, sizeof(chain));
	if (cs != 0) {
		fprintf(stderr, "error transmit chain: %d", cs);
		return 0;
	}
	_wave_wait();
	
	gpioWaveDelete(wave_begin);
	gpioWaveDelete(wave_step);
	gpioWaveDelete(wave_end);
	if (axis->_broken) {
		return axis->_ticks/axis->_delay;
	}
	return steps;
}

int axis_move_rel(Axis *axis, int steps) {
	if (steps > 0) {
		return _axis_move_rel(axis, steps, 1);
	} else {
		return -_axis_move_rel(axis, -steps, 0);
	}
}

int axis_move(Axis *axis, int pos) {
	return axis_move_rel(axis, pos - axis->position);
}
