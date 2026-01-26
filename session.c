/*
 * session.c - Session management for a4 terminal multiplexer
 *
 * Provides detach/reattach functionality similar to abduco.
 */
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <pwd.h>
#include <signal.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sys/ioctl.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>

#if defined(__linux__) || defined(__CYGWIN__)
#include <pty.h>
#elif defined(__FreeBSD__) || defined(__DragonFly__)
#include <libutil.h>
#elif defined(__OpenBSD__) || defined(__NetBSD__) || defined(__APPLE__)
#include <util.h>
#endif

#include "session.h"

#define FD_SET_MAX(fd, set, maxfd) do { \
		FD_SET(fd, set);        \
		if (fd > maxfd)         \
			maxfd = fd;     \
	} while (0)

/* Static globals for server/client state */
static SessionServer server = { .running = true, .exit_status = -1 };
static struct sockaddr_un sockaddr = { .sun_family = AF_UNIX };
static char saved_socket_path[PATH_MAX];  /* Saved copy for cleanup */
static struct termios orig_term, cur_term;
static bool has_term = false;
static bool alternate_buffer = false;
static char detach_key = DEFAULT_DETACH_KEY;

/* Forward declarations */
static void session_info(const char *str, ...);
static void session_die(const char *s);
static ssize_t write_all(int fd, const char *buf, size_t len);
static ssize_t read_all(int fd, char *buf, size_t len);
static int connect_to_session(const char *name);
static int server_create_socket(const char *name);
static int set_socket_non_blocking(int sock);
static void server_sigterm_handler(int sig);
static void server_sigchld_handler(int sig);
static void client_sigwinch_handler(int sig);
static void client_setup_terminal(void);
static void client_restore_terminal(void);
static int client_mainloop(void);

/* Packet helpers */
static inline size_t packet_header_size(void) {
	return offsetof(Packet, u);
}

static size_t packet_size(Packet *pkt) {
	return packet_header_size() + pkt->len;
}

void session_config_init(SessionConfig *cfg) {
	cfg->name = NULL;
	cfg->detach_key = DEFAULT_DETACH_KEY;
}

static ssize_t write_all(int fd, const char *buf, size_t len) {
	ssize_t ret = len;
	while (len > 0) {
		ssize_t res = write(fd, buf, len);
		if (res < 0) {
			if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)
				continue;
			return -1;
		}
		if (res == 0)
			return ret - len;
		buf += res;
		len -= res;
	}
	return ret;
}

static ssize_t read_all(int fd, char *buf, size_t len) {
	ssize_t ret = len;
	while (len > 0) {
		ssize_t res = read(fd, buf, len);
		if (res < 0) {
			if (errno == EWOULDBLOCK)
				return ret - len;
			if (errno == EAGAIN || errno == EINTR)
				continue;
			return -1;
		}
		if (res == 0)
			return ret - len;
		buf += res;
		len -= res;
	}
	return ret;
}

bool session_send_packet(int socket, Packet *pkt) {
	size_t size = packet_size(pkt);
	if (size > sizeof(*pkt))
		return false;
	return write_all(socket, (char *)pkt, size) == (ssize_t)size;
}

bool session_recv_packet(int socket, Packet *pkt) {
	ssize_t len = read_all(socket, (char *)pkt, packet_header_size());
	if (len <= 0 || len != (ssize_t)packet_header_size())
		return false;
	if (pkt->len > sizeof(pkt->u.msg)) {
		pkt->len = 0;
		return false;
	}
	if (pkt->len > 0) {
		len = read_all(socket, pkt->u.msg, pkt->len);
		if (len <= 0 || len != (ssize_t)pkt->len)
			return false;
	}
	return true;
}

static void session_info(const char *str, ...) {
	va_list ap;
	va_start(ap, str);
	if (str) {
		fprintf(stderr, "a4: %s: ", server.session_name ? server.session_name : "session");
		vfprintf(stderr, str, ap);
		fprintf(stderr, "\r\n");
		fflush(stderr);
	}
	va_end(ap);
}

static void session_die(const char *s) {
	perror(s);
	exit(EXIT_FAILURE);
}

bool session_get_socket_dir(char *path, size_t size) {
	const char *state_home = getenv("XDG_STATE_HOME");

	if (state_home && state_home[0]) {
		snprintf(path, size, "%s/a4", state_home);
	} else {
		const char *home = getenv("HOME");
		if (!home || !home[0]) {
			struct passwd *pw = getpwuid(getuid());
			if (pw)
				home = pw->pw_dir;
		}
		if (!home || !home[0]) {
			errno = ENOENT;
			return false;
		}
		snprintf(path, size, "%s/.local/state/a4", home);
	}

	/* Create directory hierarchy if needed */
	struct stat sb;
	if (stat(path, &sb) == -1) {
		/* Create parent directories */
		char parent[PATH_MAX];
		snprintf(parent, sizeof(parent), "%s", path);
		char *p = parent + 1;
		while ((p = strchr(p, '/'))) {
			*p = '\0';
			if (mkdir(parent, S_IRWXU) == -1 && errno != EEXIST)
				return false;
			*p++ = '/';
		}
		/* Create the final directory */
		if (mkdir(path, S_IRWXU) == -1 && errno != EEXIST)
			return false;
	} else if (!S_ISDIR(sb.st_mode)) {
		errno = ENOTDIR;
		return false;
	}

	/* Verify ownership and permissions */
	if (stat(path, &sb) == -1)
		return false;
	if (sb.st_uid != getuid()) {
		errno = EACCES;
		return false;
	}

	return true;
}

bool session_get_socket_path(char *path, size_t size, const char *name) {
	char dir[PATH_MAX];
	if (!session_get_socket_dir(dir, sizeof(dir)))
		return false;

	/* Session name can be a simple name or absolute path */
	if (name[0] == '/') {
		if (strlen(name) >= size) {
			errno = ENAMETOOLONG;
			return false;
		}
		strncpy(path, name, size);
	} else {
		if (snprintf(path, size, "%s/%s", dir, name) >= (int)size) {
			errno = ENAMETOOLONG;
			return false;
		}
	}
	return true;
}

static int connect_to_session(const char *name) {
	int fd;
	struct stat sb;

	if (!session_get_socket_path(sockaddr.sun_path, sizeof(sockaddr.sun_path), name))
		return -1;

	if ((fd = socket(AF_UNIX, SOCK_STREAM, 0)) == -1)
		return -1;

	socklen_t socklen = offsetof(struct sockaddr_un, sun_path) + strlen(sockaddr.sun_path) + 1;
	if (connect(fd, (struct sockaddr *)&sockaddr, socklen) == -1) {
		if (errno == ECONNREFUSED && stat(sockaddr.sun_path, &sb) == 0 && S_ISSOCK(sb.st_mode))
			unlink(sockaddr.sun_path);
		close(fd);
		return -1;
	}
	return fd;
}

pid_t session_get_pid(const char *name) {
	Packet pkt;
	pid_t pid = 0;
	int sock = connect_to_session(name);
	if (sock == -1)
		return 0;
	if (session_recv_packet(sock, &pkt) && pkt.type == MSG_PID)
		pid = pkt.u.l;
	close(sock);
	return pid;
}

bool session_exists(const char *name) {
	struct stat sb;
	pid_t pid = session_get_pid(name);
	if (!pid)
		return false;
	char path[PATH_MAX];
	if (!session_get_socket_path(path, sizeof(path), name))
		return false;
	return stat(path, &sb) == 0 && S_ISSOCK(sb.st_mode);
}

static int server_create_socket(const char *name) {
	if (!session_get_socket_path(sockaddr.sun_path, sizeof(sockaddr.sun_path), name))
		return -1;

	int fd = socket(AF_UNIX, SOCK_STREAM, 0);
	if (fd == -1)
		return -1;

	socklen_t socklen = offsetof(struct sockaddr_un, sun_path) + strlen(sockaddr.sun_path) + 1;
	mode_t mask = umask(S_IXUSR | S_IRWXG | S_IRWXO);
	int r = bind(fd, (struct sockaddr *)&sockaddr, socklen);
	umask(mask);

	if (r == -1) {
		close(fd);
		return -1;
	}

	if (listen(fd, 5) == -1) {
		unlink(sockaddr.sun_path);
		close(fd);
		return -1;
	}

	return fd;
}

static int set_socket_non_blocking(int sock) {
	int flags;
	if ((flags = fcntl(sock, F_GETFL, 0)) == -1)
		flags = 0;
	return fcntl(sock, F_SETFL, flags | O_NONBLOCK);
}

static void server_atexit_handler(void) {
	if (saved_socket_path[0])
		unlink(saved_socket_path);
}

static void server_sigterm_handler(int sig) {
	exit(EXIT_FAILURE); /* invoke atexit handler */
}

static void server_sigchld_handler(int sig) {
	int errsv = errno;
	pid_t pid;

	while ((pid = waitpid(-1, &server.exit_status, WNOHANG)) != 0) {
		if (pid == -1)
			break;
		server.exit_status = WEXITSTATUS(server.exit_status);
		server.running = false;
	}
	errno = errsv;
}

static SessionClient *client_malloc(int socket) {
	SessionClient *c = calloc(1, sizeof(SessionClient));
	if (!c)
		return NULL;
	c->socket = socket;
	return c;
}

static void client_free(SessionClient *c) {
	if (c && c->socket > 0)
		close(c->socket);
	free(c);
}

static void server_accept_client(void) {
	int newfd = accept(server.socket, NULL, NULL);
	if (newfd == -1 || set_socket_non_blocking(newfd) == -1) {
		if (newfd != -1)
			close(newfd);
		return;
	}

	SessionClient *c = client_malloc(newfd);
	if (!c) {
		close(newfd);
		return;
	}

	c->socket = newfd;
	c->state = STATE_CONNECTED;
	c->next = server.clients;
	server.clients = c;
	server.read_pty = true;

	/* Send our PID to the client */
	Packet pkt = {
		.type = MSG_PID,
		.len = sizeof(pkt.u.l),
		.u.l = getpid(),
	};
	session_send_packet(c->socket, &pkt);
}

static bool server_read_pty(Packet *pkt) {
	pkt->type = MSG_CONTENT;
	ssize_t len = read(server.pty, pkt->u.msg, sizeof(pkt->u.msg));
	if (len > 0)
		pkt->len = len;
	else if (len == 0)
		server.running = false;
	else if (len == -1 && errno != EAGAIN && errno != EINTR && errno != EWOULDBLOCK)
		server.running = false;
	return len > 0;
}

static bool server_write_pty(Packet *pkt) {
	size_t size = pkt->len;
	if (write_all(server.pty, pkt->u.msg, size) == (ssize_t)size)
		return true;
	server.running = false;
	return false;
}

void session_server_loop(SessionServer *srv) {
	atexit(server_atexit_handler);

	fd_set new_readfds;
	FD_ZERO(&new_readfds);
	FD_SET(srv->socket, &new_readfds);
	int new_fdmax = srv->socket;

	if (srv->read_pty)
		FD_SET_MAX(srv->pty, &new_readfds, new_fdmax);

	while (srv->running || srv->clients) {
		int fdmax = new_fdmax;
		fd_set readfds = new_readfds;
		FD_SET_MAX(srv->socket, &readfds, fdmax);

		/* If not running, use a short timeout to avoid blocking forever */
		struct timeval tv = { .tv_sec = 0, .tv_usec = 10000 }; /* 10ms */
		struct timeval *timeout = srv->running ? NULL : &tv;

		if (select(fdmax + 1, &readfds, NULL, NULL, timeout) == -1) {
			if (errno == EINTR)
				continue;
			session_die("server-mainloop");
		}

		FD_ZERO(&new_readfds);
		new_fdmax = srv->socket;

		bool pty_data = false;
		Packet server_packet, client_packet;

		if (FD_ISSET(srv->socket, &readfds))
			server_accept_client();

		if (FD_ISSET(srv->pty, &readfds))
			pty_data = server_read_pty(&server_packet);

		for (SessionClient **prev_next = &srv->clients, *c = srv->clients; c;) {
			if (FD_ISSET(c->socket, &readfds)) {
				if (session_recv_packet(c->socket, &client_packet)) {
					switch (client_packet.type) {
					case MSG_CONTENT:
						server_write_pty(&client_packet);
						break;
					case MSG_ATTACH:
						/* All clients have equal access */
						break;
					case MSG_RESIZE:
						c->state = STATE_ATTACHED;
						/* First client controls terminal size */
						if (c == srv->clients) {
							struct winsize ws = { 0 };
							ws.ws_row = client_packet.u.ws.rows;
							ws.ws_col = client_packet.u.ws.cols;
							ioctl(srv->pty, TIOCSWINSZ, &ws);
						}
						kill(-srv->pid, SIGWINCH);
						break;
					case MSG_EXIT:
					case MSG_DETACH:
						c->state = STATE_DISCONNECTED;
						break;
					default:
						break;
					}
				} else {
					/* recv failed - client disconnected */
					c->state = STATE_DISCONNECTED;
				}
			}

			if (c->state == STATE_DISCONNECTED) {
				bool first = (c == srv->clients);
				SessionClient *t = c->next;
				client_free(c);
				*prev_next = c = t;
				if (first && srv->clients) {
					/* Ask new first client to resize */
					Packet pkt = {
						.type = MSG_RESIZE,
						.len = 0,
					};
					session_send_packet(srv->clients->socket, &pkt);
				}
				continue;
			}

			FD_SET_MAX(c->socket, &new_readfds, new_fdmax);

			if (pty_data)
				session_send_packet(c->socket, &server_packet);

			if (!srv->running && srv->exit_status != -1) {
				Packet pkt = {
					.type = MSG_EXIT,
					.u.i = srv->exit_status,
					.len = sizeof(pkt.u.i),
				};
				session_send_packet(c->socket, &pkt);
				shutdown(c->socket, SHUT_WR); /* Signal EOF to client */
				c->state = STATE_DISCONNECTED;
				continue;
			}
			prev_next = &c->next;
			c = c->next;
		}

		if (srv->running && srv->read_pty)
			FD_SET_MAX(srv->pty, &new_readfds, new_fdmax);
	}

	exit(EXIT_SUCCESS);
}

static bool client_need_resize = false;

static void client_sigwinch_handler(int sig) {
	client_need_resize = true;
}

static void client_restore_terminal(void) {
	if (!has_term)
		return;
	tcsetattr(STDIN_FILENO, TCSAFLUSH, &orig_term);
	if (alternate_buffer) {
		printf("\033[?25h\033[?1049l");
		fflush(stdout);
		alternate_buffer = false;
	}
}

static void client_setup_terminal(void) {
	if (!has_term)
		return;
	atexit(client_restore_terminal);

	cur_term = orig_term;
	cur_term.c_iflag &= ~(IGNBRK | BRKINT | PARMRK | ISTRIP | INLCR | IGNCR | ICRNL | IXON | IXOFF);
	cur_term.c_oflag &= ~(OPOST);
	cur_term.c_lflag &= ~(ECHO | ECHONL | ICANON | ISIG | IEXTEN);
	cur_term.c_cflag &= ~(CSIZE | PARENB);
	cur_term.c_cflag |= CS8;
	cur_term.c_cc[VLNEXT] = _POSIX_VDISABLE;
	cur_term.c_cc[VMIN] = 1;
	cur_term.c_cc[VTIME] = 0;
	tcsetattr(STDIN_FILENO, TCSANOW, &cur_term);

	if (!alternate_buffer) {
		printf("\033[?1049h\033[H");
		fflush(stdout);
		alternate_buffer = true;
	}
}

static int client_mainloop(void) {
	sigset_t emptyset, blockset;
	sigemptyset(&emptyset);
	sigemptyset(&blockset);
	sigaddset(&blockset, SIGWINCH);
	sigprocmask(SIG_BLOCK, &blockset, NULL);

	client_need_resize = true;
	Packet pkt = {
		.type = MSG_ATTACH,
		.u.i = 0,  /* No flags - equal access for all */
		.len = sizeof(pkt.u.i),
	};
	session_send_packet(server.socket, &pkt);

	while (server.running) {
		fd_set fds;
		FD_ZERO(&fds);
		FD_SET(STDIN_FILENO, &fds);
		FD_SET(server.socket, &fds);

		if (client_need_resize) {
			struct winsize ws;
			if (ioctl(STDIN_FILENO, TIOCGWINSZ, &ws) != -1) {
				Packet pkt = {
					.type = MSG_RESIZE,
					.u = { .ws = { .rows = ws.ws_row, .cols = ws.ws_col } },
					.len = sizeof(pkt.u.ws),
				};
				if (session_send_packet(server.socket, &pkt))
					client_need_resize = false;
			}
		}

		struct timespec timeout = { .tv_sec = 0, .tv_nsec = 100000000 }; /* 100ms */
		int ret = pselect(server.socket + 1, &fds, NULL, NULL, &timeout, &emptyset);
		if (ret == -1) {
			if (errno == EINTR)
				continue;
			session_die("client-mainloop");
		}
		if (ret == 0)
			continue; /* timeout - check loop condition and retry */

		if (FD_ISSET(server.socket, &fds)) {
			Packet pkt;
			if (session_recv_packet(server.socket, &pkt)) {
				switch (pkt.type) {
				case MSG_CONTENT:
					write_all(STDOUT_FILENO, pkt.u.msg, pkt.len);
					break;
				case MSG_RESIZE:
					client_need_resize = true;
					break;
				case MSG_EXIT:
					session_send_packet(server.socket, &pkt);
					close(server.socket);
					return pkt.u.i;
				default:
					break;
				}
			} else {
				/* recv failed - server disconnected */
				close(server.socket);
				return -EIO;
			}
		}

		if (FD_ISSET(STDIN_FILENO, &fds)) {
			Packet pkt = { .type = MSG_CONTENT };
			ssize_t len = read(STDIN_FILENO, pkt.u.msg, sizeof(pkt.u.msg));
			if (len == -1 && errno != EAGAIN && errno != EINTR)
				session_die("client-stdin");
			if (len > 0) {
				pkt.len = len;
				if (pkt.u.msg[0] == detach_key) {
					pkt.type = MSG_DETACH;
					pkt.len = 0;
					session_send_packet(server.socket, &pkt);
					close(server.socket);
					return -1;
				}
				if (!session_send_packet(server.socket, &pkt)) {
					/* Server is gone */
					close(server.socket);
					return -EIO;
				}
			} else if (len == 0) {
				return -1;
			}
		}
	}

	return -EIO;
}

static bool create_session(const char *name, char *const argv[]) {
	/*
	 * Double fork strategy to daemonize the server:
	 * 1. First fork creates intermediate process
	 * 2. Intermediate forks again and exits, orphaning the server
	 * 3. Server uses forkpty to create PTY and spawn a4
	 * 4. Original process gets signaled when server is ready
	 */
	int client_pipe[2], server_pipe[2];
	pid_t pid;
	char errormsg[255];
	struct sigaction sa;

	if (session_exists(name)) {
		errno = EADDRINUSE;
		return false;
	}

	if (pipe(client_pipe) == -1)
		return false;

	if ((server.socket = server_create_socket(name)) == -1)
		return false;

	switch ((pid = fork())) {
	case 0: /* child process */
		setsid();
		close(client_pipe[0]);

		switch ((pid = fork())) {
		case 0: /* grandchild = server process */
			if (pipe(server_pipe) == -1) {
				snprintf(errormsg, sizeof(errormsg), "server-pipe: %s\n", strerror(errno));
				write_all(client_pipe[1], errormsg, strlen(errormsg));
				close(client_pipe[1]);
				_exit(EXIT_FAILURE);
			}

			sa.sa_flags = 0;
			sigemptyset(&sa.sa_mask);
			sa.sa_handler = server_sigchld_handler;
			sigaction(SIGCHLD, &sa, NULL);

			/* forkpty creates PTY and forks - child gets the PTY as stdin/stdout */
			switch (server.pid = forkpty(&server.pty, NULL,
			                             has_term ? &server.term : NULL,
			                             &server.winsize)) {
			case 0: /* great-grandchild = a4 application process */
				close(server.socket);
				close(server_pipe[0]);
				/* Unset A4 env var so a4 can run inside session */
				unsetenv("A4");
				/* Set A4_SESSION to session name so inner a4 knows not to use ini session */
				setenv("A4_SESSION", name, 1);
				if (fcntl(client_pipe[1], F_SETFD, FD_CLOEXEC) == 0 &&
				    fcntl(server_pipe[1], F_SETFD, FD_CLOEXEC) == 0)
					execvp(argv[0], argv);
				snprintf(errormsg, sizeof(errormsg), "server-execvp: %s: %s\n",
				         argv[0], strerror(errno));
				write_all(client_pipe[1], errormsg, strlen(errormsg));
				write_all(server_pipe[1], errormsg, strlen(errormsg));
				close(client_pipe[1]);
				close(server_pipe[1]);
				_exit(EXIT_FAILURE);
				break;

			case -1: /* forkpty failed */
				snprintf(errormsg, sizeof(errormsg), "server-forkpty: %s\n", strerror(errno));
				write_all(client_pipe[1], errormsg, strlen(errormsg));
				close(client_pipe[1]);
				close(server_pipe[0]);
				close(server_pipe[1]);
				_exit(EXIT_FAILURE);
				break;

			default: /* grandchild = server process continues */
				sa.sa_handler = server_sigterm_handler;
				sigaction(SIGTERM, &sa, NULL);
				sigaction(SIGINT, &sa, NULL);
				sa.sa_handler = SIG_IGN;
				sigaction(SIGPIPE, &sa, NULL);
				sigaction(SIGHUP, &sa, NULL);

				/* Save socket path for cleanup before chdir */
				strncpy(saved_socket_path, sockaddr.sun_path, sizeof(saved_socket_path));

				if (chdir("/") == -1)
					_exit(EXIT_FAILURE);

#ifdef NDEBUG
				/* Redirect stdio to /dev/null for daemonized server */
				int fd = open("/dev/null", O_RDWR);
				if (fd != -1) {
					dup2(fd, STDIN_FILENO);
					dup2(fd, STDOUT_FILENO);
					dup2(fd, STDERR_FILENO);
					close(fd);
				}
#endif
				close(client_pipe[1]);
				close(server_pipe[1]);

				/* Wait for exec to complete (pipe closes on success) */
				if (read_all(server_pipe[0], errormsg, sizeof(errormsg)) > 0)
					_exit(EXIT_FAILURE);
				close(server_pipe[0]);

				/* Run the server main loop */
				server.read_pty = true;
				session_server_loop(&server);
				break;
			}
			break;

		case -1: /* fork failed */
			snprintf(errormsg, sizeof(errormsg), "server-fork: %s\n", strerror(errno));
			write_all(client_pipe[1], errormsg, strlen(errormsg));
			close(client_pipe[1]);
			_exit(EXIT_FAILURE);
			break;

		default: /* intermediate process - just exit */
			close(client_pipe[1]);
			_exit(EXIT_SUCCESS);
			break;
		}
		break;

	case -1: /* fork failed */
		close(client_pipe[0]);
		close(client_pipe[1]);
		return false;

	default: /* parent = original client process */
		close(client_pipe[1]);
		while (waitpid(pid, NULL, 0) == -1 && errno == EINTR)
			;
		ssize_t len = read_all(client_pipe[0], errormsg, sizeof(errormsg));
		if (len > 0) {
			write_all(STDERR_FILENO, errormsg, len);
			unlink(sockaddr.sun_path);
			exit(EXIT_FAILURE);
		}
		close(client_pipe[0]);
	}

	return true;
}

int session_list(void) {
	char dir[PATH_MAX];
	if (!session_get_socket_dir(dir, sizeof(dir))) {
		fprintf(stderr, "a4: cannot access session directory\n");
		return 1;
	}

	DIR *dp = opendir(dir);
	if (!dp) {
		if (errno == ENOENT) {
			printf("No active sessions\n");
			return 0;
		}
		perror("opendir");
		return 1;
	}

	printf("Active sessions in %s:\n", dir);

	struct dirent *entry;
	int count = 0;
	while ((entry = readdir(dp)) != NULL) {
		if (entry->d_name[0] == '.')
			continue;

		char path[PATH_MAX];
		if (snprintf(path, sizeof(path), "%s/%s", dir, entry->d_name) >= (int)sizeof(path)) {
			fprintf(stderr, "warning: session path too long: %s/%s\n", dir, entry->d_name);
			continue;
		}

		struct stat sb;
		if (stat(path, &sb) != 0 || !S_ISSOCK(sb.st_mode))
			continue;

		/* Check if session is alive */
		pid_t pid = session_get_pid(entry->d_name);
		if (!pid)
			continue;

		/* Format modification time */
		char timebuf[64];
		struct tm *tm = localtime(&sb.st_mtime);
		strftime(timebuf, sizeof(timebuf), "%Y-%m-%d %H:%M:%S", tm);

		printf("  %s\t%d\t%s\n", timebuf, (int)pid, entry->d_name);
		count++;
	}

	closedir(dp);

	if (count == 0)
		printf("  (no active sessions)\n");

	return 0;
}

int session_connect(const char *name, char key) {
	detach_key = key;
	server.session_name = name;

	if (server.socket > 0)
		close(server.socket);

	if ((server.socket = connect_to_session(name)) == -1)
		return -1;

	if (set_socket_non_blocking(server.socket) == -1)
		return -1;

	struct sigaction sa;
	sa.sa_flags = 0;
	sigemptyset(&sa.sa_mask);
	sa.sa_handler = client_sigwinch_handler;
	sigaction(SIGWINCH, &sa, NULL);
	sa.sa_handler = SIG_IGN;
	sigaction(SIGPIPE, &sa, NULL);

	client_setup_terminal();
	int status = client_mainloop();
	client_restore_terminal();

	if (status == -1) {
		session_info("detached");
	} else if (status == -EIO) {
		session_info("exited due to I/O errors");
	} else {
		session_info("session terminated with exit status %d", status);
	}

	return status;
}

int session_main(SessionConfig *cfg, int argc, char *argv[]) {
	server.session_name = cfg->name;
	detach_key = cfg->detach_key;

	if (tcgetattr(STDIN_FILENO, &orig_term) != -1) {
		server.term = orig_term;
		has_term = true;
	}

	if (ioctl(STDIN_FILENO, TIOCGWINSZ, &server.winsize) == -1) {
		server.winsize.ws_col = 80;
		server.winsize.ws_row = 25;
	}

	/* Build argv for a4 without session flags (it runs inside the session) */
	char *a4_argv[argc + 1];
	int a4_argc = 0;
	a4_argv[a4_argc++] = argv[0];

	/* Copy non-session arguments */
	for (int i = 1; i < argc; i++) {
		/* Skip session-related arguments */
		if (strcmp(argv[i], "-a") == 0) {
			i++; /* Skip the session name argument too */
			continue;
		}
		a4_argv[a4_argc++] = argv[i];
	}
	a4_argv[a4_argc] = NULL;

	/* Attach to existing session or create new one */
	if (session_exists(cfg->name))
		return session_connect(cfg->name, cfg->detach_key);

	if (!create_session(cfg->name, a4_argv))
		session_die("create-session");

	return session_connect(cfg->name, cfg->detach_key);
}
