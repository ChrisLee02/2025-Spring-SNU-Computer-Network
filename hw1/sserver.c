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

void kill_all_children() {
  for (int i = 0; i < N_CHILD; i++) {
    if (child_pids[i] > 0) {
      printf("killed child %d\n", child_pids[i]);  // for debug
      kill(child_pids[i], SIGKILL);
    }
  }
}

void sigchld_handler(int signo) {
  printf("SIGCHLD received\n");
  kill_all_children();
}
void sigint_handler(int signo) {
  printf("SIGINT received\n");
  kill_all_children();
}

void child_loop(int listen_fd) {
  int client_fd;
  char* buffer = malloc(MAX_HDR + MAX_CONT + 10);

  if (buffer == NULL) {
    fprintf(stderr, "malloc failed\n");
    exit(EXIT_FAILURE);
  }

  while (1) {
    if (accept(listen_fd, NULL, NULL) < 0) {
      fprintf(stderr, "accept() failed\n");
      free(buffer);
      exit(EXIT_FAILURE);
    }
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

  int server_fd, client_fd;
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
