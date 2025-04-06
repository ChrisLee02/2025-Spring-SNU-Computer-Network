#include <arpa/inet.h>
#include <assert.h>
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/sendfile.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h> /* See NOTES */
#include <sys/wait.h>
#include <unistd.h>

#include "macro.h"
#define MAX_VAL (MAX_HDR)
#define MAX_URL 1024
#define EVENT_NUM 128
#define CRLF "\r\n"
#define CRLFCRLF "\r\n\r\n"

// ============================================================
// === HTTP Parser Functions ===
// ============================================================

// enum for code
// ok, 400, 403, 404
typedef enum {
  OK = 200,
  BAD_REQUEST = 400,
  FORBIDDEN = 403,
  NOT_FOUND = 404,
  INTERNAL_SERVER_ERROR = 500,
} res_code;

typedef enum { RESP_WRITING_HEADER, RESP_WRITING_BODY } write_state;

typedef enum { KEEP_ALIVE, CLOSE } connection;

typedef struct {
  char url[MAX_URL];
  char version[10];
  connection keep_alive;
} http_request;

/* Skip spaces by moving the pointer's position */
void skip_blank(const char** p) {
  while (isblank(**p)) {
    (*p)++;
  }
}

/* Parse first line of header, which is case-sensitive.
   Return 0 on success, -1 on failure. */
int parse_first_line(const char** p, http_request* req) {
  if (strncmp(*p, "GET", 3) != 0) {
    return -1;
  }

  *p += 3;

  skip_blank(p);

  // starts with "/" and save it to req.url
  if (**p != '/') {
    return -1;
  }
  int i = 0;
  while (isgraph(**p)) {
    req->url[i++] = **p;
    (*p)++;
  }

  skip_blank(p);

  if (strncmp(*p, "HTTP/1.0", 8) == 0) {
    req->keep_alive = CLOSE;
  } else if (strncmp(*p, "HTTP/1.1", 8) == 0) {
    req->keep_alive = KEEP_ALIVE;
  } else {
    return -1;
  }

  *p += 8;

  skip_blank(p);

  if (strncmp(*p, CRLF, 2) != 0) {
    return -1;
  }

  *p += 2;

  return 0;
}

/* Parse one line of header, suppose it is host header.
   Return 0 on success, -1 on failure. */
int parse_host(const char** p) {
  if (strncasecmp(*p, "host:", 5) != 0) {
    return -1;
  }

  *p += 5;

  skip_blank(p);

  /* string should exist before CRLF */
  if (!isgraph(**p)) {
    return -1;
  }

  while (isgraph(**p)) {
    (*p)++;
  }

  skip_blank(p);

  if (strncmp(*p, CRLF, 2) != 0) {
    return -1;
  }

  *p += 2;

  return 0;
}

/* Parse connection header.
   Return 0 on success, -1 on failure. */
int parse_connection(const char** p, http_request* req) {
  if (strncasecmp(*p, "connection:", 11) != 0) {
    return -1;
  }

  *p += 11;

  skip_blank(p);

  if (strncasecmp(*p, "keep-alive", 10) == 0) {
    req->keep_alive = KEEP_ALIVE;
    *p += 10;
  } else if (strncasecmp(*p, "close", 5) == 0) {
    req->keep_alive = CLOSE;
    *p += 5;
  } else {
    return -1;
  }

  skip_blank(p);

  if (strncmp(*p, CRLF, 2) != 0) {
    return -1;
  }

  *p += 2;

  return 0;
}

/* Parse header, get data and save it.
   Return OK on success, BAD_REQUEST on failure. */
res_code parse_request(const char* header_buf, http_request* req) {
  const char* p = header_buf;
  if (parse_first_line(&p, req) < 0) {
    return BAD_REQUEST;
  }

  int host_parsed = 0;

  while (1) {
    if (strncasecmp(p, "host:", 5) == 0) {
      if (parse_host(&p) < 0) {
        return BAD_REQUEST;
      }
      host_parsed = 1;
    } else if (strncasecmp(p, "connection:", 11) == 0) {
      if (parse_connection(&p, req) < 0) {
        return BAD_REQUEST;
      }
    } else {
      if (strstr(p, CRLF) == NULL) {
        return BAD_REQUEST;
      }
      p = strstr(p, CRLF) + 2;
    }
    if (strncmp(p, CRLF, 2) == 0) {
      break;
    }
  }

  if (!host_parsed) {
    return BAD_REQUEST;
  }
  return OK;
}

// ============================================================
// === Connection Management Functions ===
// ============================================================

typedef struct {
  int fd;
  char read_buffer[MAX_HDR];
  size_t read_len;
  http_request request;

  int file_fd;
  off_t file_offset;
  size_t file_remain;

  write_state write_phase;

  char header_buf[MAX_HDR];
  size_t header_len;
  size_t header_sent;
} connection_context;

static const char* g_rootDir = "./"; /* root directory */

connection_context* create_connection_context(int fd) {
  connection_context* ctx = malloc(sizeof(connection_context));
  if (!ctx) {
    fprintf(stderr, "malloc failed in create_connection_context\n");
    return NULL;
  }

  memset(ctx, 0, sizeof(connection_context));
  ctx->fd = fd;
  ctx->file_fd = -1;
  return ctx;
}

void reset_context(connection_context* ctx) {
  // reset the context except for fd and keep_alive
  memset(&ctx->request, 0, sizeof(http_request));
  memset(ctx->read_buffer, 0, MAX_HDR);
  memset(ctx->header_buf, 0, MAX_HDR);
  ctx->read_len = 0;
  if (ctx->file_fd >= 0) close(ctx->file_fd);
  ctx->file_fd = -1;
  ctx->file_offset = 0;
  ctx->file_remain = 0;
  ctx->write_phase = RESP_WRITING_HEADER;
  ctx->header_len = 0;
  ctx->header_sent = 0;
}

void close_connection(connection_context* ctx, int epoll_fd) {
  epoll_ctl(epoll_fd, EPOLL_CTL_DEL, ctx->fd, NULL);
  close(ctx->fd);
  if (ctx->file_fd >= 0) close(ctx->file_fd);
  free(ctx);
}

// ============================================================
// === Response Handling Functions ===
// ============================================================

const char* errMessage400 =
    "HTTP/1.0 400 Bad Request\r\n"
    "Connection: close\r\n"
    "\r\n";

const char* errMessage403 =
    "HTTP/1.0 403 Forbidden\r\n"
    "Connection: close\r\n"
    "\r\n";

const char* errMessage404 =
    "HTTP/1.0 404 Not Found\r\n"
    "Connection: close\r\n"
    "\r\n";

const char* errMessage500 =
    "HTTP/1.0 500 Internal Server Error"
    "\r\nConnection: close\r\n"
    "\r\n";

void send_error(connection_context* ctx, res_code err_code, int epoll_fd) {
  const char* msg;
  switch (err_code) {
    case BAD_REQUEST:
      msg = errMessage400;
      break;
    case FORBIDDEN:
      msg = errMessage403;
      break;
    case NOT_FOUND:
      msg = errMessage404;
      break;
    case INTERNAL_SERVER_ERROR:
    default:
      msg = errMessage500;
      break;
  }

  size_t len = strlen(msg);
  memcpy(ctx->header_buf, msg, len);
  ctx->header_len = len;
  ctx->header_sent = 0;
  ctx->file_remain = 0;
  ctx->write_phase = RESP_WRITING_HEADER;
  ctx->request.keep_alive = CLOSE;

  struct epoll_event ev = {.events = EPOLLOUT | EPOLLET, .data.ptr = ctx};
  if (epoll_ctl(epoll_fd, EPOLL_CTL_MOD, ctx->fd, &ev) < 0) {
    fprintf(stderr, "epoll_ctl_mod failed in send_error\n");
    close_connection(ctx, epoll_fd);
  }
}

res_code prepare_response(connection_context* ctx) {
  char file_path[MAX_URL * 2];

  snprintf(file_path, sizeof(file_path), "%s%s", g_rootDir, ctx->request.url);

  struct stat st;
  if (stat(file_path, &st) < 0) {
    if (errno == ENOENT) {
      return NOT_FOUND;
    }
    return INTERNAL_SERVER_ERROR;
  }

  if (!S_ISREG(st.st_mode)) {
    return FORBIDDEN;
  }

  if (access(file_path, R_OK) < 0) {
    return FORBIDDEN;
  }

  int fd = open(file_path, O_RDONLY);
  if (fd < 0) {
    return INTERNAL_SERVER_ERROR;
  }

  ctx->file_fd = fd;
  ctx->file_offset = 0;
  ctx->file_remain = st.st_size;
  ctx->write_phase = RESP_WRITING_HEADER;

  int len =
      snprintf(ctx->header_buf, MAX_HDR,
               "HTTP/1.0 200 OK\r\n"
               "Content-length: %ld\r\n"
               "Connection: %s\r\n"
               "\r\n",
               st.st_size,
               ctx->request.keep_alive == KEEP_ALIVE ? "keep-alive" : "close");

  ctx->header_len = len;
  ctx->header_sent = 0;

  return OK;
}

// ============================================================
// === Epoll Event Handling Functions ===
// ============================================================

void handle_new_connection(int listen_fd, int epoll_fd) {
  int client_fd = accept(listen_fd, NULL, NULL);
  if (client_fd < 0) {
    fprintf(stderr, "accept failed\n");
    return;
  }

  if (set_nonblocking(client_fd) < 0) {
    fprintf(stderr, "set_nonblocking failed\n");
    close(client_fd);
    return;
  }

  connection_context* ctx = create_connection_context(client_fd);
  if (!ctx) {
    close(client_fd);
    return;
  }

  struct epoll_event ev = {.events = EPOLLIN | EPOLLET, .data.ptr = ctx};

  if (epoll_ctl(epoll_fd, EPOLL_CTL_ADD, client_fd, &ev) < 0) {
    fprintf(stderr, "epoll_ctl_add failed for new client\n");
    close_connection(ctx, epoll_fd);
  }
}

void handle_epollin(connection_context* ctx, int epoll_fd) {
  while (1) {
    ssize_t n = read(ctx->fd, ctx->read_buffer + ctx->read_len,
                     MAX_HDR - ctx->read_len);
    if (n < 0) {
      if (errno == EAGAIN || errno == EWOULDBLOCK) return;
      fprintf(stderr, "error from reading client\n");
      perror("read from client:");
      close_connection(ctx, epoll_fd);
      return;
    } else if (n == 0) {
      close_connection(ctx, epoll_fd);
      return;
    }

    ctx->read_len += n;
    ctx->read_buffer[ctx->read_len] = '\0';
    if (strstr(ctx->read_buffer, CRLFCRLF) != NULL) {
      res_code parse_result = parse_request(ctx->read_buffer, &ctx->request);
      if (parse_result != OK) {
        send_error(ctx, parse_result, epoll_fd);
        return;
      }

      res_code file_access = prepare_response(ctx);
      if (file_access != OK) {
        send_error(ctx, file_access, epoll_fd);
        return;
      }

      struct epoll_event ev = {.events = EPOLLOUT | EPOLLET, .data.ptr = ctx};
      if (epoll_ctl(epoll_fd, EPOLL_CTL_MOD, ctx->fd, &ev) < 0) {
        fprintf(stderr, "epoll_ctl_mod to EPOLLOUT failed\n");
        send_error(ctx, INTERNAL_SERVER_ERROR, epoll_fd);
        return;
      }

      break;
    }

    if (ctx->read_len == MAX_HDR) {
      send_error(ctx, BAD_REQUEST, epoll_fd);
      return;
    }
  }
}

/* Handle EPOLLOUT events by sending header and body */
void handle_epollout(connection_context* ctx, int epoll_fd) {
  if (ctx->write_phase == RESP_WRITING_HEADER) {
    while (ctx->header_sent < ctx->header_len) {
      ssize_t sent = write(ctx->fd, ctx->header_buf + ctx->header_sent,
                           ctx->header_len - ctx->header_sent);
      if (sent < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
          return;
        }
        fprintf(stderr, "write error (header)\n");
        close_connection(ctx, epoll_fd);
        return;
      }
      ctx->header_sent += sent;
    }
    ctx->write_phase = RESP_WRITING_BODY;
  }

  if (ctx->write_phase == RESP_WRITING_BODY) {
    while (ctx->file_remain > 0) {
      ssize_t sent =
          sendfile(ctx->fd, ctx->file_fd, &ctx->file_offset, ctx->file_remain);
      if (sent < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
          return;
        }
        fprintf(stderr, "sendfile error (body)\n");
        close_connection(ctx, epoll_fd);
        return;
      }
      ctx->file_remain -= sent;
    }

    // When body is sent completely
    if (ctx->request.keep_alive == KEEP_ALIVE) {
      reset_context(ctx);
      struct epoll_event ev = {.events = EPOLLIN | EPOLLET, .data.ptr = ctx};
      if (epoll_ctl(epoll_fd, EPOLL_CTL_MOD, ctx->fd, &ev) < 0) {
        fprintf(stderr, "epoll_ctl_mod back to EPOLLIN failed\n");
        close_connection(ctx, epoll_fd);
      }
    } else {
      close_connection(ctx, epoll_fd);
    }
  }
}

// ============================================================
// === Main Function and Initialization ===
// ============================================================

static void PrintUsage(const char* prog) {
  printf("usage: %s -p port -d rootDirectory(optional) \n", prog);
}

int set_nonblocking(int sockfd) {
  int flags = fcntl(sockfd, F_GETFL, 0);  // get default flags
  if (flags == -1) return -1;
  return fcntl(sockfd, F_SETFL, flags | O_NONBLOCK);  // set as non-blocking
}

int main(const int argc, const char** argv) {
  int i;
  int port = -1;

  /* argument parsing */
  for (i = 1; i < argc; i++) {
    if (strcmp(argv[i], "-p") == 0 && (i + 1) < argc) {
      port = atoi(argv[i + 1]);
      i++;
    } else if (strcmp(argv[i], "-d") == 0 && (i + 1) < argc) {
      g_rootDir = argv[i + 1];
      i++;
    }
  }
  if (port <= 0 || port > 65535) {
    PrintUsage(argv[0]);
    exit(-1);
  }
  if (access(g_rootDir, R_OK | X_OK) < 0) {
    fprintf(stderr, "root dir %s inaccessible, errno=%d\n", g_rootDir, errno);
    PrintUsage(argv[0]);
    exit(-1);
  }

  /* ignore SIGPIPE */
  signal(SIGPIPE, SIG_IGN);

  // implement your own code

  int listen_fd;
  struct sockaddr_in server_addr;

  if ((listen_fd = socket(AF_INET, SOCK_STREAM, 0)) < 0) {
    fprintf(stderr, "socket failed for listen_fd\n");
    exit(-1);
  }

  memset(&server_addr, 0, sizeof(server_addr));
  server_addr.sin_family = AF_INET;
  server_addr.sin_addr.s_addr = htonl(INADDR_ANY);
  server_addr.sin_port = htons(port);
  if (bind(listen_fd, (struct sockaddr*)&server_addr, sizeof(server_addr)) <
      0) {
    fprintf(stderr, "bind failed for listen_fd\n");
    close(listen_fd);
    exit(-1);
  }
  if (listen(listen_fd, 1024) < 0) {
    fprintf(stderr, "listen failed for listen_fd\n");
    close(listen_fd);
    exit(-1);
  }

  if (set_nonblocking(listen_fd) < 0) {
    fprintf(stderr, "set_nonblocking failed for listen_fd\n");
    close(listen_fd);
    exit(-1);
  }

  int epoll_fd = epoll_create1(0);
  if (epoll_fd < 0) {
    fprintf(stderr, "epoll_create1 failed\n");
    close(listen_fd);
    exit(-1);
  }
  struct epoll_event ev, events[EVENT_NUM];
  ev.events = EPOLLIN;
  ev.data.fd = listen_fd;
  if (epoll_ctl(epoll_fd, EPOLL_CTL_ADD, listen_fd, &ev) < 0) {
    fprintf(stderr, "epoll_ctl failed for listen_fd\n");
    close(listen_fd);
    close(epoll_fd);
    exit(-1);
  }

  while (1) {
    int nfds = epoll_wait(epoll_fd, events, EVENT_NUM, -1);
    if (nfds < 0) {
      // todo: handle interrupted system call by signal
      /*  if (errno == EINTR) {
         continue;
       } */
      fprintf(stderr, "epoll_wait failed\n");
      break;
    }
    for (int i = 0; i < nfds; i++) {
      if (events[i].data.fd == listen_fd) {
        handle_new_connection(listen_fd, epoll_fd);
      } else {
        // handle client request
        connection_context* ctx = (connection_context*)events[i].data.ptr;
        if (events[i].events & EPOLLIN) {
          handle_epollin(ctx, epoll_fd);
        }
        if (events[i].events & EPOLLOUT) {
          handle_epollout(ctx, epoll_fd);
        }
      }
    }
  }
}
