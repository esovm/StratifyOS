/* Copyright 2011-2017 Tyler Gilbert;
 * This file is part of Stratify OS.
 *
 * Stratify OS is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * Stratify OS is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with Stratify OS.  If not, see <http://www.gnu.org/licenses/>.
 *
 *
 */


#include <stdlib.h>
#include <sos/fs/sysfs.h>
#include <string.h>

#include "../scheduler/scheduler_local.h"
#include "mcu/core.h"
#include "cortexm/mpu.h"
#include "cortexm/task.h"
#include "mcu/debug.h"

typedef struct {
	int code_size;
	int data_size;
} priv_load_data_t;

static void priv_load_data(void * args) MCU_PRIV_EXEC_CODE;
void priv_load_data(void * args){

	priv_load_data_t * p = args;
	int size;
	void * code_addr;
	uint32_t code_size;
	void * src_addr;
	void * dest_addr;

	//sanity check the size
	size =  mpu_size((uint32_t)task_table[ task_get_current() ].mem.data.size);
	if( p->data_size < size ){
		size = p->data_size;
	}

	code_size = p->code_size;
	if( code_size > mpu_size((uint32_t)task_table[ task_get_current() ].mem.code.size) ){
		return;
	}

	dest_addr = mpu_addr((uint32_t)task_table[ task_get_current() ].mem.data.addr);
	code_addr = mpu_addr((uint32_t)task_table[ task_get_current() ].mem.code.addr);
	code_size = p->code_size;
	src_addr = code_addr + code_size;
	memcpy(dest_addr, src_addr, size);
}

void crt_load_data(void * global_reent, int code_size, int data_size){
	priv_load_data_t args;
	args.code_size = code_size;
	args.data_size = data_size;
	cortexm_svcall(priv_load_data, &args);
}

char ** const crt_import_argv(char * path_arg, int * argc){
	char ** argv;
	char * arg_buffer;
	char * p;
	char * next;

	int count;
	int len;

	if( path_arg == 0 ){
		*argc = 0;
		return 0;
	}

	len = strlen(path_arg);


	arg_buffer = malloc(len);
	if( arg_buffer == 0 ){
		//since we couldn't allocate memory in the application, free the memory allocated on global
		_free_r(task_table[0].global_reent, path_arg);
		return 0;
	}

	strcpy(arg_buffer, path_arg);

	count = 0;
	next = strtok_r(path_arg, sysfs_whitespace, &p);
	while( next ){
		next = strtok_r(0, sysfs_whitespace, &p);
		count++;
	}

	//free the path_arg passed from shared system memory
	_free_r(task_table[0].global_reent, path_arg);

	argv = malloc(sizeof(char*)*count);
	if( argv == 0 ){
		return 0;
	}

	count = 0;
	argv[0] = strtok_r(arg_buffer, sysfs_whitespace, &p);
	while(argv[count] != 0 ){
		count++;
		argv[count] = strtok_r(0, sysfs_whitespace, &p);
	}

	*argc = count;
	return argv;
}

