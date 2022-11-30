#define _POSIX_C_SOURCE 200809L
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#include "log.h"
#include "sway-ipc.h"

static const char sway_ipc_magic[] = {'i', '3', '-', 'i', 'p', 'c'};

#define SWAY_IPC_HEADER_SIZE (6 + 4 + 4)

char *get_sway_socket_path(void) {
	const char *swaysock = getenv("SWAYSOCK");
	if (swaysock) {
		return strdup(swaysock);
	}
	char *line = NULL;
	size_t line_size = 0;
	FILE *fp = popen("sway --get-socketpath 2>/dev/null", "r");
	if (fp) {
		ssize_t nret = getline(&line, &line_size, fp);
		pclose(fp);
		if (nret > 0) {
			// remove trailing newline, if there is one
			if (line[nret - 1] == '\n') {
				line[nret - 1] = '\0';
			}
			return line;
		}
	}
	const char *i3sock = getenv("I3SOCK");
	if (i3sock) {
		free(line);
		return strdup(i3sock);
	}
	fp = popen("i3 --get-socketpath 2>/dev/null", "r");
	if (fp) {
		ssize_t nret = getline(&line, &line_size, fp);
		pclose(fp);
		if (nret > 0) {
			// remove trailing newline, if there is one
			if (line[nret - 1] == '\n') {
				line[nret - 1] = '\0';
			}
			return line;
		}
	}
	free(line);
	return NULL;
}

int sway_ipc_open_socket(const char *socket_path) {
	struct sockaddr_un addr;
	int socketfd;
	if ((socketfd = socket(AF_UNIX, SOCK_STREAM, 0)) == -1) {
		wsbg_log_errno(LOG_ERROR, "Unable to open Unix socket");
		return -1;
	}
	addr.sun_family = AF_UNIX;
	strncpy(addr.sun_path, socket_path, sizeof(addr.sun_path) - 1);
	addr.sun_path[sizeof(addr.sun_path) - 1] = 0;
	int l = sizeof(struct sockaddr_un);
	if (connect(socketfd, (struct sockaddr *)&addr, l) == -1) {
		wsbg_log_errno(LOG_ERROR, "Unable to connect to %s", socket_path);
		return -1;
	}
	return socketfd;
}

void sway_ipc_open(struct sway_ipc_state *state) {
	state->fd = -1;
	state->buffer = NULL;
	state->buffer_size = 0;
	state->received = 0;
	state->payload_size = 0;

	char *socket_path = get_sway_socket_path();
	if (!socket_path) {
		wsbg_log(LOG_ERROR, "Unable to retrieve Sway socket path");
		return;
	}

	state->fd = sway_ipc_open_socket(socket_path);
	free(socket_path);
	if (state->fd == -1) {
		return;
	}

	if (fcntl(state->fd, F_SETFL, O_NONBLOCK) == -1) {
		wsbg_log_errno(LOG_ERROR, "Unable to set Sway socket to be non-blocking");
		sway_ipc_close(state);
	}
}

void sway_ipc_close(struct sway_ipc_state *state) {
	if (state->fd == -1) {
		return;
	}
	if (state->buffer) {
		free(state->buffer);
	}
	if (close(state->fd) == -1) {
		wsbg_log_errno(LOG_ERROR, "Unable to close Sway socket");
	}
	state->fd = -1;
}

bool sway_ipc_recv(struct sway_ipc_state *state, struct sway_ipc_message *message) {
	if (state->fd == -1) {
		return false;
	}

	size_t size = SWAY_IPC_HEADER_SIZE + state->payload_size;
	if (state->buffer_size < size) {
		char *buffer = realloc(state->buffer, size);
		if (!buffer) {
			wsbg_log_errno(LOG_ERROR, "Unable to allocate memory for Sway IPC response");
			sway_ipc_close(state);
			return false;
		}
		state->buffer = buffer;
		state->buffer_size = size;
	}

	ssize_t received = recv(state->fd,
			state->buffer + state->received, size - state->received, 0);
	if (received == -1) {
		if (errno == EAGAIN || errno == EWOULDBLOCK) {
			return false;
		}
		wsbg_log_errno(LOG_ERROR, "Unable to receive Sway IPC message");
		sway_ipc_close(state);
		return false;
	}

	state->received += received;
	if (state->received < SWAY_IPC_HEADER_SIZE) {
		return false;
	}

	if (state->payload_size == 0) {
		memcpy(&state->payload_size,
				state->buffer + sizeof sway_ipc_magic,
				sizeof state->payload_size);
#if (SIZE_MAX - SWAY_IPC_HEADER_SIZE) <= UINT32_MAX
		if ((SIZE_MAX - SWAY_IPC_HEADER_SIZE) < state->payload_size) {
			wsbg_log(LOG_ERROR, "Sway IPC message payload too big");
			sway_ipc_close(state);
			return false;
		}
#endif
		if (state->payload_size != 0) {
			return sway_ipc_recv(state, message);
		}
	}

	if (state->received != size) {
		return false;
	}

	message->size = state->payload_size;
	memcpy(&message->type,
			state->buffer + sizeof sway_ipc_magic + sizeof message->size,
			sizeof message->type);
	message->payload = &state->buffer[SWAY_IPC_HEADER_SIZE];

	state->received = 0;
	state->payload_size = 0;
	return true;
}

bool sway_ipc_send_data(struct sway_ipc_state *state,
		const char *buffer, size_t length, int timeout) {
	while (true) {
		ssize_t sent = send(state->fd, buffer, length, 0);
		if (sent != -1) {
			if ((length -= sent) == 0) {
				return true;
			}
			buffer += sent;
		} else if (errno == EAGAIN || errno == EWOULDBLOCK) {
			struct pollfd pfd = {
				.fd = state->fd,
				.events = POLLOUT
			};
			int n;
			while ((n = poll(&pfd, 1, timeout)) == -1) {
				if (errno == EINTR) {
					continue;
				}
				goto error;
			}
			if (n == 0) {
				errno = ETIMEDOUT;
				goto error;
			}
		} else {
			goto error;
		}
	}
error:
	wsbg_log_errno(LOG_ERROR, "Unable to send Sway IPC command");
	sway_ipc_close(state);
	return false;
}

void sway_ipc_send(struct sway_ipc_state *state,
		uint32_t type, const char *payload) {
	if (state->fd == -1) {
		return;
	}

	uint32_t len = payload ? strlen(payload) : 0;
	char data[SWAY_IPC_HEADER_SIZE];
	memcpy(data, sway_ipc_magic, sizeof(sway_ipc_magic));
	memcpy(data + sizeof(sway_ipc_magic), &len, sizeof(len));
	memcpy(data + sizeof(sway_ipc_magic) + sizeof(len), &type, sizeof(type));

	if (sway_ipc_send_data(state, data, SWAY_IPC_HEADER_SIZE, -1) &&
			payload && len != 0) {
		sway_ipc_send_data(state, payload, len, -1);
	}
}
