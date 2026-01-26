/*
 * session.h - Session management for a4 terminal multiplexer
 *
 * Provides detach/reattach functionality similar to abduco.
 * Uses Unix domain sockets for client-server communication.
 *
 * Socket location: $XDG_STATE_HOME/a4 or ~/.local/state/a4
 */
#ifndef SESSION_H
#define SESSION_H

#include <stdbool.h>
#include <stdint.h>
#include <sys/ioctl.h>
#include <sys/types.h>
#include <termios.h>

#ifndef CTRL
#define CTRL(k) ((k) & 0x1F)
#endif

/* Default detach key: Ctrl-\ */
#define DEFAULT_DETACH_KEY CTRL('\\')

/* Packet types for client-server communication */
enum PacketType {
	MSG_CONTENT = 0,  /* Terminal content data */
	MSG_ATTACH  = 1,  /* Client attach request */
	MSG_DETACH  = 2,  /* Client detach request */
	MSG_RESIZE  = 3,  /* Window size change */
	MSG_EXIT    = 4,  /* Server exit notification */
	MSG_PID     = 5,  /* Server PID notification */
};

/* Packet structure for communication (4KB fixed size) */
typedef struct {
	uint32_t type;
	uint32_t len;
	union {
		char msg[4096 - 2 * sizeof(uint32_t)];
		struct {
			uint16_t rows;
			uint16_t cols;
		} ws;
		uint32_t i;
		uint64_t l;
	} u;
} Packet;

/* Client connection state */
typedef struct SessionClient SessionClient;
struct SessionClient {
	int socket;
	enum {
		STATE_CONNECTED,
		STATE_ATTACHED,
		STATE_DETACHED,
		STATE_DISCONNECTED,
	} state;
	bool need_resize;
	SessionClient *next;
};

/* Server state */
typedef struct {
	SessionClient *clients;
	int socket;
	Packet output;
	int pty;
	int exit_status;
	struct termios term;
	struct winsize winsize;
	pid_t pid;
	volatile sig_atomic_t running;
	const char *session_name;
	bool read_pty;
} SessionServer;

/* Session configuration passed from main */
typedef struct {
	const char *name;    /* Session name */
	char detach_key;     /* Key to trigger detach */
} SessionConfig;

/* Function declarations */

/* Initialize session configuration with defaults */
void session_config_init(SessionConfig *cfg);

/* Get the socket directory path (creates if needed) */
bool session_get_socket_dir(char *path, size_t size);

/* Get full socket path for a session name */
bool session_get_socket_path(char *path, size_t size, const char *name);

/* Check if a session exists and is alive */
bool session_exists(const char *name);
pid_t session_get_pid(const char *name);

/* Create a new session (server side) */
bool session_create(const char *name, SessionServer *server);

/* Attach to an existing session (client side) */
int session_connect(const char *name, char detach_key);

/* List available sessions */
int session_list(void);

/* Main entry point - handles session logic based on config */
int session_main(SessionConfig *cfg, int argc, char *argv[]);

/* Server main loop (called after fork) */
void session_server_loop(SessionServer *server);

/* Packet I/O helpers */
bool session_send_packet(int socket, Packet *pkt);
bool session_recv_packet(int socket, Packet *pkt);

#endif /* SESSION_H */
