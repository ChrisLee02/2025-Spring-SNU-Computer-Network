/*
 * testClientConcurrent.c
 *
 * This program tests the server’s ability to handle concurrent connections by
 * creating 5 child processes that each send a request with a 10MB body.
 *
 * Usage:
 *   ./testClientConcurrent -p port -s server-ip
 *
 * Each child process connects to the server, sends a request with a 10MB body,
 * receives the response, prints it, and then exits.
 */

#include <arpa/inet.h>
#include <netdb.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/types.h> /* See NOTES */
#include <sys/wait.h>
#include <unistd.h>

#include "macro.h"  // Defines MAX_CONT as (10 * 1024 * 1024)

#define CRLF "\r\n"
#define CRLFCRLF "\r\n\r\n"
#define NUM_CONCURRENT 5

/* write_all: writes total bytes from buf to fd */
ssize_t write_all(int fd, const char* buf, ssize_t total) {
  ssize_t bytes_written = 0;
  while (bytes_written < total) {
    ssize_t res = write(fd, buf + bytes_written, total - bytes_written);
    if (res < 0) {
      return res;
    } else if (res == 0) {
      break;
    }
    bytes_written += res;
  }
  return bytes_written;
}

/* read_all: reads up to max_read_size bytes from fd into buf */
ssize_t read_all(int fd, char* buf, ssize_t max_read_size) {
  ssize_t bytes_read = 0;
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

void client_task(const char* server, int port, int client_no) {
  int socketfd;
  struct hostent* hp;
  struct sockaddr_in saddr;
  char header[MAX_HDR + 10];
  char* buffer = NULL;
  char* body = NULL;
  ssize_t req_body_size, req_header_size, bytes_written;

  /* Allocate and generate a 10MB body filled with 'A' characters */
  req_body_size = MAX_CONT;  // 10 MB (as defined in macro.h)
  body = malloc(req_body_size + 1);
  if (!body) {
    fprintf(stderr, "client %d: malloc for body failed\n", client_no);
    exit(EXIT_FAILURE);
  }
  memset(body, 'A', req_body_size);
  body[req_body_size] = '\0';  // NULL-terminate for header formatting if needed

  /* Create socket */
  if ((socketfd = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP)) < 0) {
    fprintf(stderr, "client %d: socket() failed\n", client_no);
    free(body);
    exit(EXIT_FAILURE);
  }

  if ((hp = gethostbyname(server)) == NULL) {
    fprintf(stderr, "client %d: gethostbyname() failed\n", client_no);
    close(socketfd);
    free(body);
    exit(EXIT_FAILURE);
  }

  memset(&saddr, 0, sizeof(saddr));
  saddr.sin_family = AF_INET;
  memcpy(&saddr.sin_addr.s_addr, hp->h_addr_list[0], hp->h_length);
  saddr.sin_port = htons(port);

  if (connect(socketfd, (struct sockaddr*)&saddr, sizeof(saddr)) < 0) {
    fprintf(stderr, "client %d: connect() failed\n", client_no);
    close(socketfd);
    free(body);
    exit(EXIT_FAILURE);
  }

  /* Build request header */
  req_header_size = snprintf(header, MAX_HDR,
                             "POST message SIMPLE/1.0\r\n"
                             "Host: %s\r\n"
                             "Content-length: %ld\r\n"
                             "\r\n",
                             server, req_body_size);
  if (req_header_size < 0) {
    fprintf(stderr, "client %d: snprintf() failed\n", client_no);
    close(socketfd);
    free(body);
    exit(EXIT_FAILURE);
  }
  if (req_header_size > MAX_HDR) {
    fprintf(stderr, "client %d: too large header\n", client_no);
    close(socketfd);
    free(body);
    exit(EXIT_FAILURE);
  }

  /* Send header */
  bytes_written = write_all(socketfd, header, req_header_size);
  if (bytes_written < 0 || bytes_written < req_header_size) {
    fprintf(stderr, "client %d: write header failed\n", client_no);
    close(socketfd);
    free(body);
    exit(EXIT_FAILURE);
  }

  /* Send 10MB body */
  bytes_written = write_all(socketfd, body, req_body_size);
  if (bytes_written < 0 || bytes_written < req_body_size) {
    fprintf(stderr, "client %d: write body failed\n", client_no);
    close(socketfd);
    free(body);
    exit(EXIT_FAILURE);
  }
  // shutdown(socketfd, SHUT_WR);

  /* Allocate buffer for response */
  buffer = malloc(MAX_HDR + MAX_CONT + 10);
  if (!buffer) {
    fprintf(stderr, "client %d: malloc for response buffer failed\n",
            client_no);
    close(socketfd);
    free(body);
    exit(EXIT_FAILURE);
  }

  ssize_t res_size = read_all(socketfd, buffer, MAX_HDR + MAX_CONT);
  if (res_size < 0) {
    fprintf(stderr, "client %d: read response failed\n", client_no);
    free(buffer);
    close(socketfd);
    free(body);
    exit(EXIT_FAILURE);
  }
  buffer[res_size] = '\0';  // Null-terminate for string processing

  // printf("client %d received response:\n%s\n", client_no, buffer);
  printf("client %d received response:\n", client_no);

  free(buffer);
  free(body);
  close(socketfd);
  exit(EXIT_SUCCESS);
}

int main(int argc, char** argv) {
  const char* server = NULL;
  int port = -1;
  int i;

  /* Process command-line arguments: -p port, -s server-ip */
  for (i = 1; i < argc; i++) {
    if (strcmp(argv[i], "-p") == 0 && (i + 1) < argc) {
      port = atoi(argv[i + 1]);
      i++;
    } else if (strcmp(argv[i], "-s") == 0 && (i + 1) < argc) {
      server = argv[i + 1];
      i++;
    }
  }

  if (port < 0 || server == NULL) {
    fprintf(stderr, "Usage: %s -p port -s server-ip\n", argv[0]);
    exit(EXIT_FAILURE);
  }
  if (port < 1024 || port > 65535) {
    fprintf(stderr, "Port number should be between 1024 and 65535.\n");
    exit(EXIT_FAILURE);
  }

  pid_t pids[NUM_CONCURRENT];

  /* Fork NUM_CONCURRENT (5) child processes to send requests concurrently */
  for (int j = 0; j < NUM_CONCURRENT; j++) {
    pids[j] = fork();
    if (pids[j] < 0) {
      perror("fork");
      exit(EXIT_FAILURE);
    } else if (pids[j] == 0) {
      /* Child process: execute client_task */
      client_task(server, port, j);
      // client_task() calls exit() internally.
    }
  }

  /* Parent process: wait for all children to exit */
  for (int j = 0; j < NUM_CONCURRENT; j++) {
    waitpid(pids[j], NULL, 0);
  }

  return EXIT_SUCCESS;
}
