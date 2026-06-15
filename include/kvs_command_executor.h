#ifndef KVS_COMMAND_EXECUTOR_H
#define KVS_COMMAND_EXECUTOR_H

#include "kvs_command_task.h"
#include "kvs_response_task.h"

int kvs_command_executor_start(void);
void kvs_command_executor_stop(void);

int kvs_command_executor_submit(kvs_command_task_t *task);
int kvs_command_executor_poll_response(kvs_response_task_t **resp);
int kvs_command_executor_response_event_fd(void);

#endif /* KVS_COMMAND_EXECUTOR_H */
