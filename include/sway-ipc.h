#ifndef _WSBG_SWAY_IPC_H
#define _WSBG_SWAY_IPC_H

#include <stdbool.h>
#include <stdint.h>

// i3 command types
#define SWAY_IPC_GET_WORKSPACES            1
#define SWAY_IPC_SUBSCRIBE                 2
#define SWAY_IPC_GET_OUTPUTS               3

// Events sent from sway to clients. Events have the highest bits set.
#define SWAY_IPC_EVENT_WORKSPACE  0x80000000

/**
 * Sway IPC state including socket file descriptor,
 * message buffer, and recv state.
 */
struct sway_ipc_state {
	int fd;
	char *buffer;
	size_t buffer_size;
	size_t received;
	uint32_t payload_size;
};

/**
 * IPC message including type of message, size of payload
 * and the json encoded payload string.
 */
struct sway_ipc_message {
	uint32_t size;
	uint32_t type;
	char *payload;
};

/**
 * Opens the Sway socket.
 */
void sway_ipc_open(struct sway_ipc_state *state);
/**
 * Closes the Sway socket and frees resources.
 * This function can safely be called multiple times.
 */
void sway_ipc_close(struct sway_ipc_state *state);
/**
 * Issues a single IPC command.
 * If there is an error, the Sway socket will be closed.
 */
void sway_ipc_send(struct sway_ipc_state *state,
		uint32_t type, const char *payload);
/**
 * Receives a single IPC response.
 * If there is a response ready, returns `true` and stores
 * the response in `message`. If no response is ready,
 * or if there is an error, returns `false`.
 * This function does not block.
 * If there is an error, the Sway socket will be closed.
 */
bool sway_ipc_recv(struct sway_ipc_state *state,
		struct sway_ipc_message *message);

#endif
