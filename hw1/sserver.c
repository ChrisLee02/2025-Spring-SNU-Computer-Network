#include <arpa/inet.h>
#include <assert.h>
#include <ctype.h>
#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/types.h> /* See NOTES */
#include <sys/wait.h>
#include <unistd.h>

#include "macro.h"

#define CRLF "\r\n"
#define CRLFCRLF "\r\n\r\n"

#define N_CHILD 5

pid_t child_pids[N_CHILD];

/* kill all children */
void kill_all_children() {
  for (int i = 0; i < N_CHILD; i++) {
    if (child_pids[i] > 0) {
      printf("killed child %d\n", child_pids[i]);  // for debug
      kill(child_pids[i], SIGKILL);
    }
  }
}

/* If any child is terminated or interupt received, kill remaining children and
  terminate parent. */
void sigchld_handler(int signo) {
  printf("SIGCHLD received\n");
  kill_all_children();
}
void sigint_handler(int signo) {
  printf("SIGINT received\n");
  kill_all_children();
}

/* write [total] bytes from buf to fd
   return value may be less than total
   if write() returns 0
   return -1 on error
*/
ssize_t write_all(int fd, const void* buf, size_t total) {
  size_t bytes_written = 0;
  while (bytes_written < total) {
    ssize_t res = write(fd, buf + bytes_written, total - bytes_written);
    if (res < 0) {
      return res;
    } else if (res == 0) {
      break;
    }
    bytes_written += res;
  }
  return total;
}

/* read [max_read_size] bytes from fd to buf
   return value may be less than max_read_size
   if EOF is reached
   return -1 on error
*/
ssize_t read_all(int fd, void* buf, size_t max_read_size) {
  size_t bytes_read = 0;
  while (bytes_read < max_read_size) {
    ssize_t res = read(fd, buf + bytes_read, max_read_size - bytes_read);
    if (res < 0) {
      return res;
    }
    if (res == 0) {
      break;
    }
    bytes_read += res;
  }
  return bytes_read;
}

/* send response of error code 400  */
void send_400(int fd) {
  char* response = "SIMPLE/1.0 400 Bad Request\r\n\r\n";
  write_all(fd, response, strlen(response));
}

/* skip space, by moving pointer's position */
void skip_blank(const char** p) {
  while (isblank(**p)) {
    (*p)++;
  }
}

/* parse first line of header, which is case-sensitive
    return 0 on success, -1 on failure
*/
int parse_first_line(const char** p) {
  if (strncmp(*p, "POST", 4) != 0) {
    return -1;
  }

  *p += 4;

  skip_blank(p);

  if (strncmp(*p, "message", 7) != 0) {
    return -1;
  }

  *p += 7;

  skip_blank(p);

  if (strncmp(*p, "SIMPLE/1.0", 10) != 0) {
    return -1;
  }

  *p += 10;

  skip_blank(p);

  if (strncmp(*p, CRLF, 2) != 0) {
    return -1;
  }

  *p += 2;

  return 0;
}

/* parse one line of header, suppose it is host header
    return 0 on success, -1 on failure
*/
int parse_host(const char** p) {
  if (strncasecmp(*p, "Host:", 5) != 0) {
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

/* parse one line of header, suppose it is content-length header
    return content-length on success, -1 on failure
*/
int parse_content_length(const char** p) {
  if (strncasecmp(*p, "Content-Length:", 15) != 0) {
    return -1;
  }

  *p += 15;

  skip_blank(p);

  if (!isdigit(**p)) {
    return -1;
  }

  char* num_end;
  long content_length = strtol(*p, &num_end, 10);

  if (content_length > MAX_CONT) {
    return -1;
  }

  *p = num_end;

  skip_blank(p);

  if (strncmp(*p, CRLF, 2) != 0) {
    return -1;
  }

  *p += 2;

  return content_length;
}

/* parse header, get content_length and save it
   return 0 on success, -1 on failure
*/
int parse_header(const char* header, int* content_length) {
  const char* p = header;

  if (parse_first_line(&p) < 0) {
    return -1;
  }

  /*
    if there're more header than 2, logic should be changed using something
    like flags.
  */
  if (strncasecmp(p, "host:", 5) == 0) {
    if (parse_host(&p) < 0) {
      return -1;
    }

    *content_length = parse_content_length(&p);

    if (*content_length < 0) {
      return -1;
    }

    // check whether there is no more header
    if (strncmp(p, CRLF, 2) != 0) {
      return -1;
    }

    p += 2;

  } else if (strncasecmp(p, "content-length:", 15) == 0) {
    *content_length = parse_content_length(&p);

    if (*content_length < 0) {
      return -1;
    }

    if (parse_host(&p) < 0) {
      return -1;
    }

    // check whether there is no more header
    if (strncmp(p, CRLF, 2) != 0) {
      return -1;
    }

  } else {
    return -1;
  }
  return 0;
}

void child_loop(int listen_fd) {
  int client_fd;
  char* buffer = malloc(MAX_HDR + MAX_CONT + 10);

  if (buffer == NULL) {
    fprintf(stderr, "malloc failed\n");
    exit(EXIT_FAILURE);
  }

  while (1) {
    if ((client_fd = accept(listen_fd, NULL, NULL)) < 0) {
      fprintf(stderr, "accept() failed\n");
      continue;
    }

    ssize_t req_size = read_all(client_fd, buffer, MAX_HDR + MAX_CONT);

    if (req_size < 0) {
      fprintf(stderr, "read failed\n");
      close(client_fd);
      continue;
    } else if (req_size == 0) {
      fprintf(stderr, "connection closed\n");
      close(client_fd);
      continue;
    }

    buffer[req_size] = '\0';

    char* header_end = strstr(buffer, CRLFCRLF);
    if (!header_end) {
      send_400(client_fd);
      close(client_fd);
      continue;
    }

    char* body_start = header_end + 4;
    int body_size = req_size - (body_start - buffer);

    int content_length;

    if (parse_header(buffer, &content_length) < 0) {
      send_400(client_fd);
      close(client_fd);
      continue;
    }

    /* if body size is less than content_length, server should suppose that
       there's a loss of data. thus send 400 error
    */
    if (body_size < content_length) {
      send_400(client_fd);
      close(client_fd);
      continue;
    }

    char* res_header_format =
        "SIMPLE/1.0 200 OK\r\ncContent-length: %d\r\n\r\n";
    char res_header_buf[MAX_HDR + 10];
    int res_header_size = snprintf(res_header_buf, MAX_HDR + 1,
                                   res_header_format, content_length);
    if (res_header_size < 0) {
      fprintf(stderr, "snprintf failed\n");
      close(client_fd);
      continue;
    }

    write_all(client_fd, res_header_buf, res_header_size);
    write_all(client_fd, body_start, content_length);

    close(client_fd);
  }

  exit(0);
}

/*--------------------------------------------------------------------------------*/
int main(const int argc, const char** argv) {
  int i;
  int port = -1;

  /* argument parsing */
  for (i = 1; i < argc; i++) {
    if (strcmp(argv[i], "-p") == 0 && (i + 1) < argc) {
      port = atoi(argv[i + 1]);
      i++;
    }
  }

  if (port <= 0 || port > 65535) {
    printf("usage: %s -p port\n", argv[0]);
    exit(-1);
  }

  // implement your own code
  signal(SIGPIPE, SIG_IGN);
  signal(SIGCHLD, sigchld_handler);
  signal(SIGINT, sigint_handler);

  int server_fd;
  struct sockaddr_in saddr;

  if ((server_fd = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP)) < 0) {
    fprintf(stderr, "socket() failed\n");
    return EXIT_FAILURE;
  }

  memset(&saddr, 0, sizeof(saddr));
  saddr.sin_family = AF_INET;
  saddr.sin_addr.s_addr = INADDR_ANY;
  saddr.sin_port = htons(port);

  if (bind(server_fd, (struct sockaddr*)&saddr, sizeof(saddr)) < 0) {
    fprintf(stderr, "bind() failed\n");
    close(server_fd);
    return EXIT_FAILURE;
  }

  if (listen(server_fd, 1024) < 0) {
    fprintf(stderr, "listen() failed\n");
    close(server_fd);
    return EXIT_FAILURE;
  }

  for (i = 0; i < N_CHILD; i++) {
    if ((child_pids[i] = fork()) == 0) {
      child_loop(server_fd);
    }
  }

  for (i = 0; i < N_CHILD; i++) {
    waitpid(child_pids[i], NULL, 0);
  }

  close(server_fd);
  return EXIT_SUCCESS;
}
