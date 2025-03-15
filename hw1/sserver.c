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
}
