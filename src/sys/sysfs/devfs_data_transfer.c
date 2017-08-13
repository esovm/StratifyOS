/* Copyright 2011-2016 Tyler Gilbert; 
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

/*! \addtogroup UNISTD
 * @{
 */

/*! \file */

#include <reent.h>
#include <stdarg.h>
#include <sys/stat.h>
#include <errno.h>

#include "mcu/mcu.h"
#include "../sched/sched_local.h"

#include "sos/fs/sysfs.h"
#include "devfs_local.h"

#define ARGS_READ_WRITE 0
#define ARGS_READ_READ 1
#define ARGS_READ_DONE 2

typedef struct {
	const void * device;
	devfs_async_t async;
	volatile int is_read;
	int ret;
} priv_device_data_transfer_t;


static void priv_check_op_complete(void * args);
static void priv_device_data_transfer(void * args) MCU_PRIV_EXEC_CODE;
static int priv_data_transfer_callback(void * context, const mcu_event_t * data) MCU_PRIV_CODE;
static void clear_device_action(const void * config, const devfs_device_t * device, int loc, int is_read);

int priv_data_transfer_callback(void * context, const mcu_event_t * event){
	//activate all tasks that are blocked on this signal
	int i;
	int new_priority;
	priv_device_data_transfer_t * args = (priv_device_data_transfer_t*)context;


	new_priority = -1;
	if ( (uint32_t)args->async.tid < task_get_total() ){

		if ( sched_inuse_asserted(args->async.tid) ){
			if( event->o_events & MCU_EVENT_FLAG_CANCELED ){
				args->async.nbyte = -1; //ignore any data transferred and return an error
			}
		}

		if ( sched_inuse_asserted(args->async.tid) && !sched_stopped_asserted(args->async.tid) ){ //check to see if the process terminated or stopped
			new_priority = sos_sched_table[args->async.tid].priority;
		}
	}

	//check to see if any tasks are waiting for this device
	for(i = 1; i < task_get_total(); i++){
		if ( task_enabled(i) && sched_inuse_asserted(i) ){
			if ( sos_sched_table[i].block_object == (args->device + args->is_read) ){
				sched_priv_assert_active(i, SCHED_UNBLOCK_TRANSFER);
				if( !sched_stopped_asserted(i) && (sos_sched_table[i].priority > new_priority) ){
					new_priority = sos_sched_table[i].priority;
				}
			}
		}
	}

	args->is_read = ARGS_READ_DONE;
	sched_priv_update_on_wake(new_priority);

	return 0;
}


void priv_check_op_complete(void * args){
	priv_device_data_transfer_t * p = (priv_device_data_transfer_t*)args;

	if( p->is_read != ARGS_READ_DONE ){
		if ( p->ret == 0 ){
			if ( (p->async.nbyte >= 0) //wait for the operation to complete or for data to arrive
			){
				//Block waiting for the operation to complete or new data to be ready
				sos_sched_table[ task_get_current() ].block_object = (void*)p->device + p->is_read;
				//switch tasks until a signal becomes available
				sched_priv_update_on_sleep();
			}
		} else {
			p->is_read = ARGS_READ_DONE;
		}
	}
}

void priv_device_data_transfer(void * args){
	priv_device_data_transfer_t * p = (priv_device_data_transfer_t*)args;
	const devfs_device_t * dev = p->device;

	if ( p->is_read != 0 ){
		//Read operation
		p->ret = dev->driver.read(&(dev->handle), &(p->async));
		//p->ret = p->fs->read_async(p->fs->config, p->handle, &p->op);
	} else {
		//p->ret = p->fs->write_async(p->fs->config, p->handle, &p->op);
		p->ret = dev->driver.write(&(dev->handle), &(p->async));
	}

	priv_check_op_complete(args);

}

void clear_device_action(const void * config, const devfs_device_t * device, int loc, int is_read){
	mcu_action_t action;
	memset(&action, 0, sizeof(mcu_action_t));
	if( is_read ){
		action.o_events = MCU_EVENT_FLAG_DATA_READY;
	} else {
		action.o_events = MCU_EVENT_FLAG_WRITE_COMPLETE;
	}
	action.channel = loc;
	devfs_ioctl(config, (void*)device, I_MCU_SETACTION, &action);
}


int devfs_data_transfer(const void * config, const devfs_device_t * device, int flags, int loc, void * buf, int nbyte, int is_read){
	int tmp;
	volatile priv_device_data_transfer_t args;

	if ( nbyte == 0 ){
		return 0;
	}

	args.device = device;
	if( is_read ){
		args.is_read = ARGS_READ_READ;
	} else {
		args.is_read = ARGS_READ_WRITE;
	}
	args.async.loc = loc;
	args.async.flags = flags;
	args.async.buf = buf;
	args.async.handler.callback = priv_data_transfer_callback;
	args.async.handler.context = (void*)&args;
	args.async.tid = task_get_current();

	tmp = 0;

	//privilege call for the operation
	do {

		args.ret = -101010;
		args.async.nbyte = nbyte;

		//This transfers the data
		cortexm_svcall(priv_device_data_transfer, (void*)&args);

		//We arrive here if the data is done transferring or there is no data to transfer and O_NONBLOCK is set
		//or if there was an error
		while( (sched_get_unblock_type(task_get_current()) == SCHED_UNBLOCK_SIGNAL)
				&& ((volatile int)args.is_read != ARGS_READ_DONE) ){

			if( (args.ret == 0) && (args.async.nbyte == 0) ){
				//no data was transferred
				clear_device_action(config, device, loc, args.is_read);
				errno = EINTR;
				//return the number of bytes transferred
				return -1;
			}

			//check again if the op is complete
			cortexm_svcall(priv_check_op_complete, (void*)&args);
		}


		if ( args.ret > 0 ){
			//The operation happened synchronously
			tmp = args.ret;
			break;
		} else if ( args.ret == 0 ){
			//the operation happened asynchronously
			if ( args.async.nbyte > 0 ){
				//The operation has completed and transferred args.async.nbyte bytes
				tmp = args.async.nbyte;
				break;
			} else if ( args.async.nbyte == 0 ){
				//There was no data to read/write -- try again
				if (args.async.flags & O_NONBLOCK ){
					errno = ENODATA;
					return -1;
				}

			} else if ( args.async.nbyte < 0 ){
				//there was an error executing the operation (or the operation was cancelled)
				return -1;
			}
		} else if ( args.ret < 0 ){
			//there was an error starting the operation (such as EAGAIN)
			if( args.ret == -101010 ){
				errno = ENXIO; //this is a rare/strange error where cortexm_svcall fails to run properly
			}
			return args.ret;
		}
	} while ( args.ret == 0 );


	return tmp;
}


/*! @} */

