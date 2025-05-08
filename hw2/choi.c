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

// my macro
#define NUM_BACKLOGS 1024
#define MAX_EVENTS 32
#define CTX_BUF_SIZE 4 * MAX_HDR
#define CRLF "\r\n"
#define CRLFCRLF "\r\n\r\n"

static const char *g_rootDir = "./"; /* root directory */
const char *errMessage400 =
    "HTTP/1.0 400 Bad Request\r\nConnection: close\r\n\r\n";
const char *errMessage403 =
    "HTTP/1.0 403 Forbidden\r\nConnection: close\r\n\r\n";
const char *errMessage404 =
    "HTTP/1.0 404 Not Found\r\nConnection: close\r\n\r\n";
const char *errMessage500 =
    "HTTP/1.0 500 Internal Server Error\r\nConnection: close\r\n\r\n";
// we need HTTP/1.1 string???

/*--------------------------------------------------------------------------------*/
typedef struct {
  int fd;
  // for request buffer part
  char *buf;
  size_t offset;
  // for response header part
  char *header_buf;
  int header_len;
  size_t header_offset;
  // connection type
  int connection;
  // for file send
  char file_path[MAX_URL];
  int file_fd;
  off_t send_offset;
} client_ctx;

static void PrintUsage(const char *prog) {
  printf("usage: %s -p port -d rootDirectory(optional) \n", prog);
}

// initilaize sockaddr_in struct "saddr" with given "port" number
static void set_saddr(struct sockaddr_in *saddr, int port) {
  memset(saddr, 0, sizeof(struct sockaddr_in));
  saddr->sin_family = AF_INET;
  saddr->sin_addr.s_addr = INADDR_ANY;
  saddr->sin_port = htons(port);
}

// set "fd" non-blocking
static int set_nonblocking(int fd) {
  int flags = fcntl(fd, F_GETFL, 0);
  if (flags == -1) {
    fprintf(stderr, "fcntl() failed\n");
    exit(EXIT_FAILURE);
  }
  return fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

// free ctx dynamic memories
void free_ctx(client_ctx *ctx) {
  close(ctx->fd);
  if (ctx->file_fd >= 0) close(ctx->file_fd);
  free(ctx->header_buf);
  free(ctx->buf);
  free(ctx);
}

// cleanup the context
void cleanup_ctx(int epoll_fd, client_ctx *ctx) {
  epoll_ctl(epoll_fd, EPOLL_CTL_DEL, ctx->fd, NULL);
  free_ctx(ctx);
}

// epoll_ctl with EPOLL_CTL_ADD
static int epoll_ctl_add(int epoll_fd, int fd, uint32_t events, int use_ctx) {
  struct epoll_event ev;
  ev.events = events;
  if (use_ctx == TRUE) {
    client_ctx *ctx = (client_ctx *)malloc(sizeof(client_ctx));
    if (ctx == NULL) {
      fprintf(stderr, "malloc() failed\n");
      return -1;
    }
    ctx->fd = fd;
    // it should be freed later
    ctx->buf = (char *)malloc(CTX_BUF_SIZE * sizeof(char));
    if (ctx->buf == NULL) {
      free(ctx);
      fprintf(stderr, "malloc() failed\n");
      return -1;
    }
    ctx->header_buf = (char *)malloc((MAX_HDR + 10) * sizeof(char));
    if (ctx->header_buf == NULL) {
      free(ctx->buf);
      free(ctx);
      fprintf(stderr, "malloc() failed\n");
      return -1;
    }
    ctx->header_len = 0;
    ctx->offset = 0;
    ctx->header_offset = 0;
    ctx->connection = 0;
    ev.data.ptr = ctx;
  } else {
    ev.data.fd = fd;
  }

  if (epoll_ctl(epoll_fd, EPOLL_CTL_ADD, fd, &ev) == -1) {
    if (use_ctx == TRUE) {
      client_ctx *ctx = ev.data.ptr;
      free_ctx(ctx);
    }
    fprintf(stderr, "epoll_ctl() failed\n");
    return -1;
  }
  return 0;
}

static int epoll_ctl_mod(int epoll_fd, client_ctx *ctx, uint32_t events) {
  struct epoll_event ev;
  ev.events = events;
  ev.data.ptr = ctx;

  if (epoll_ctl(epoll_fd, EPOLL_CTL_MOD, ctx->fd, &ev) == -1) {
    close(epoll_fd);
    close(ctx->fd);
    // free heap
    free(ctx->header_buf);
    free(ctx->buf);
    free(ctx);
    fprintf(stderr, "epoll_ctl() failed\n");
    return -1;
  }
  return 0;
}

// jump arbitrary number of blanks
void jump_blank(const char **p_str) {
  while (isblank(**p_str)) ++(*p_str);
}

// compare two string "s1" and "s2" case-insensitively
int strn_case_insensitive_cmp(const char *s1, const char *s2, size_t n) {
  for (int i = 0; i < n; ++i) {
    if (s1[i] == '\0' || s2[i] == '\0') return s1[i] - s2[i];
    if (toupper(s1[i]) != toupper(s2[i])) return -1;
  }
  return 0;
}

// parse GET line in header
// return 1 if HTTP 1.1
// return 0 if HTTP 1.0
// return -1 if mal-formed
int parse_get_line(const char **p_str, char *file_path) {
  if (strncmp(*p_str, "GET", 3) != 0) return -1;
  *p_str += 3;

  jump_blank(p_str);

  if (**p_str != '/') return -1;
  ++(*p_str);

  const char *file_path_start = *p_str;
  while (isgraph(**p_str)) {
    ++(*p_str);
  }
  const char *file_path_end = *p_str;
  if (file_path_end - file_path_start >= MAX_URL) return -1;
  memcpy(file_path, file_path_start, file_path_end - file_path_start);

  jump_blank(p_str);

  // version = 0 (HTTP 1.0) or 1 (HTTP 1.1)
  int version;
  if (strncmp(*p_str, "HTTP/1.0", 8) == 0)
    version = 0;
  else if (strncmp(*p_str, "HTTP/1.1", 8) == 0)
    version = 1;
  else
    return -1;
  *p_str += 8;

  jump_blank(p_str);

  if (strncmp(*p_str, CRLF, 2) != 0) return -1;
  *p_str += 2;
  return version;
}

// parse Host line in header
// return 0 if well-formed
// return -1 if mal-formed
int parse_host_line(const char **p_str) {
  if (strn_case_insensitive_cmp(*p_str, "Host:", 5) != 0) return -1;
  *p_str += 5;

  jump_blank(p_str);

  if (!isgraph(**p_str)) return -1;
  while (isgraph(**p_str)) ++(*p_str);

  jump_blank(p_str);

  if (strncmp(*p_str, CRLF, 2) != 0) return -1;
  *p_str += 2;
  return 0;
}

// parse Connection line in header
// return 1 if keep-alive
// return 0 if close
// return -1 if mal-formed
int parse_connection_line(const char **p_str) {
  if (strn_case_insensitive_cmp(*p_str, "Connection:", 11) != 0) return -1;
  *p_str += 11;

  jump_blank(p_str);

  if (!isgraph(**p_str)) return -1;

  // connection = 0 (close) or 1 (keep-alive)
  int connection;
  if (strn_case_insensitive_cmp(*p_str, "Close", 5) == 0) {
    connection = 0;
    *p_str += 5;
  } else if (strn_case_insensitive_cmp(*p_str, "Keep-alive", 10) == 0) {
    connection = 1;
    *p_str += 10;
  } else
    return -1;

  jump_blank(p_str);

  if (strncmp(*p_str, CRLF, 2) != 0) return -1;
  *p_str += 2;
  return connection;
}

int parse_header(client_ctx *ctx) {
  const char *header = ctx->buf;
  const char *p_ch = header;
  int connection;

  // first GET line
  if ((connection = parse_get_line(&p_ch, ctx->file_path)) < 0) return -1;

  int has_host = FALSE;
  // pass each line with CRLF, and find Host and Connection field
  while (1) {
    if (strn_case_insensitive_cmp(p_ch, "Host:", 5) == 0) {
      if (parse_host_line(&p_ch) < 0) return -1;
      has_host = TRUE;
    } else if (strn_case_insensitive_cmp(p_ch, "Connection:", 11) == 0) {
      if ((connection = parse_connection_line(&p_ch)) < 0) return -1;
    } else {
      // other header lines, we dont care
      p_ch = strstr(p_ch, CRLF);
      if (!p_ch) return -1;
      p_ch += 2;
    }

    if (strncmp(p_ch, CRLF, 2) == 0) {
      break;
    }
  }

  if (has_host == FALSE) {
    return -1;
  }
  return connection;
}

int write_header(client_ctx *ctx, int code, long content_len, int connection) {
  int header_len;
  if (code == 400) {
    header_len = strlen(errMessage400);
    memcpy(ctx->header_buf, errMessage400, header_len);
  } else if (code == 403) {
    header_len = strlen(errMessage403);
    memcpy(ctx->header_buf, errMessage403, header_len);
  } else if (code == 404) {
    header_len = strlen(errMessage404);
    memcpy(ctx->header_buf, errMessage404, header_len);
  } else if (code == 500) {
    header_len = strlen(errMessage500);
    memcpy(ctx->header_buf, errMessage500, header_len);
  } else if (code == 200) {
    char *str_conn;
    if (connection == 0)
      str_conn = "close";
    else if (connection == 1)
      str_conn = "keep-alive";
    else
      return -1;

    header_len = snprintf(ctx->header_buf, MAX_HDR,
                          "HTTP/1.0 200 OK\r\n"
                          "Content-length: %lu\r\n"
                          "Connection: %s\r\n"
                          "\r\n",
                          content_len, str_conn);
  } else {
    return -1;
  }
  ctx->header_len = header_len;
  ctx->header_offset = 0;
  return 0;
}
/*--------------------------------------------------------------------------------*/
int main(const int argc, const char **argv) {
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
  int listen_fd, epoll_fd, conn_fd;
  int return_check;
  int nfds;
  socklen_t sock_len;
  struct sockaddr_in srv_saddr;
  struct sockaddr_in cli_saddr;
  struct epoll_event events[MAX_EVENTS];

  if ((listen_fd = socket(AF_INET, SOCK_STREAM, 0)) < 0) {
    fprintf(stderr, "socket() failed\n");
    return EXIT_FAILURE;
  }
  set_saddr(&srv_saddr, port);

  if (bind(listen_fd, (struct sockaddr *)&srv_saddr, sizeof(srv_saddr)) < 0) {
    close(listen_fd);
    fprintf(stderr, "bind() failed\n");
    return EXIT_FAILURE;
  }
  set_nonblocking(listen_fd);

  if (listen(listen_fd, NUM_BACKLOGS) < 0) {
    close(listen_fd);
    fprintf(stderr, "listen() failed\n");
    return EXIT_FAILURE;
  }

  epoll_fd = epoll_create(1);
  return_check = epoll_ctl_add(epoll_fd, listen_fd, EPOLLIN, FALSE);
  if (return_check < 0) {
    // error
    close(epoll_fd);
    return EXIT_FAILURE;
  }

  sock_len = sizeof(cli_saddr);
  for (;;) {
    nfds = epoll_wait(epoll_fd, events, MAX_EVENTS, -1);
    if (nfds == -1) {
      close(listen_fd);
      close(epoll_fd);
      fprintf(stderr, "epoll_wait() failed\n");
      continue;
    }

    for (int ev_idx = 0; ev_idx < nfds; ++ev_idx) {
      // when listening queue
      if (events[ev_idx].data.fd == listen_fd) {
        conn_fd = accept(listen_fd, (struct sockaddr *)&cli_saddr, &sock_len);
        if (conn_fd == -1) {
          fprintf(stderr, "accept() failed\n");
          continue;
        }

        return_check = set_nonblocking(conn_fd);
        if (return_check < 0) {
          fprintf(stderr, "set_nonblocking() failed\n");
          continue;
        }
        return_check = epoll_ctl_add(epoll_fd, conn_fd, EPOLLIN, TRUE);
        if (return_check < 0) {
          fprintf(stderr, "set_nonblocking() failed\n");
          continue;
        }
      }
      // CLIENT -> SERVER
      else if (events[ev_idx].events & EPOLLIN) {
        client_ctx *ctx = (client_ctx *)events[ev_idx].data.ptr;
        conn_fd = ctx->fd;

        ssize_t received;
        while ((received = read(ctx->fd, ctx->buf + ctx->offset,
                                CTX_BUF_SIZE - ctx->offset)) > 0) {
          ctx->offset += received;
        }
        if (received == -1 && errno != EAGAIN && errno != EWOULDBLOCK) {
          fprintf(stderr, "set_nonblocking() failed\n");
          cleanup_ctx(epoll_fd, ctx);
          continue;
        }

        // header has not arrived yet
        char *header_end = strstr(ctx->buf, CRLFCRLF);
        if (header_end == NULL) {
          printf("CRLFCRLF failed\n");
          continue;
        }

        // parse
        int connection;
        connection = parse_header(ctx);
        ctx->connection = connection;
        ctx->file_fd = open(ctx->file_path, O_RDONLY);

        // pull remaining part in header buffer
        size_t header_size = header_end - ctx->buf + 4;
        size_t remain = ctx->offset - header_size;
        if (remain > 0) {
          memmove(ctx->buf, ctx->buf + header_size, remain);
        }
        ctx->offset = remain;

        // epollout setting
        epoll_ctl_mod(epoll_fd, ctx, EPOLLOUT | EPOLLET);
        ctx->send_offset = 0;
      }
      // SERVER -> CLIENT
      else if (events[ev_idx].events & EPOLLOUT) {
        client_ctx *ctx = (client_ctx *)events[ev_idx].data.ptr;
        int connection = ctx->connection;
        // make response header
        int response_code = 200;

        if (connection < 0) {
          // 400 (mal formed header)
          response_code = 400;
        } else {
          // 200
          if (ctx->file_fd < 0) {
            if (errno == EACCES)
              response_code = 403;  // forbiden
            else if (errno == ENOENT)
              response_code = 404;  // not found
            else
              response_code = 500;  // internal error
          }
        }
        // file stat check -> file size
        struct stat file_stat;
        int content_len = 0;
        if (ctx->file_fd >= 0) {
          if (fstat(ctx->file_fd, &file_stat) == 0) {
            content_len = file_stat.st_size;
          } else {
            // internal error
            response_code = 500;
          }
        }
        write_header(ctx, response_code, content_len, connection);

        // write header
        int do_continue = FALSE;
        while (ctx->header_offset < ctx->header_len) {
          ssize_t sent = write(ctx->fd, ctx->header_buf + ctx->header_offset,
                               ctx->header_len - ctx->header_offset);
          if (sent > 0) {
            ctx->header_offset += sent;
          } else if (sent < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            break;
          } else {
            // error
            cleanup_ctx(epoll_fd, ctx);
            do_continue = TRUE;
            break;
          }
        }
        // continue
        if (do_continue == TRUE) continue;

        // sendfile
        off_t total_size = file_stat.st_size;
        while (ctx->send_offset < total_size) {
          ssize_t sent = sendfile(ctx->fd, ctx->file_fd, &(ctx->send_offset),
                                  total_size - ctx->send_offset);
          if (sent > 0)
            continue;
          else if (sent == -1 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            break;
          } else {
            // sendfile 실패
            cleanup_ctx(epoll_fd, ctx);
            do_continue = TRUE;
            break;
          }
        }
        // continue
        if (do_continue == TRUE) continue;

        close(ctx->file_fd);
        // close connection
        if (ctx->connection == 0) {
          cleanup_ctx(epoll_fd, ctx);
        }  // keep-alive connection
        else {
          // change to EPOLLIN
          return_check = epoll_ctl_mod(epoll_fd, ctx, EPOLLIN);
          if (return_check < 0) {
            cleanup_ctx(epoll_fd, ctx);
            continue;
          }
        }
      }
    }
  }
}
