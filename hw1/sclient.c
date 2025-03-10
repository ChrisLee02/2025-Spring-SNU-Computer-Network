#include <arpa/inet.h>
#include <ctype.h>
#include <errno.h>
#include <netdb.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/types.h> /* See NOTES */
#include <unistd.h>

#include "macro.h"
/*--------------------------------------------------------------------------------*/
int main(const int argc, const char** argv) {
  const char* pserver = NULL;
  int port = -1;
  int i;

  /* argument processing */
  for (i = 1; i < argc; i++) {
    if (strcmp(argv[i], "-p") == 0 && (i + 1) < argc) {
      port = atoi(argv[i + 1]);
      i++;
    } else if (strcmp(argv[i], "-s") == 0 && (i + 1) < argc) {
      pserver = argv[i + 1];
      i++;
    }
  }

  /* check arguments */
  if (port < 0 || pserver == NULL) {
    printf("usage: %s -p port -s server-ip\n", argv[0]);
    exit(-1);
  }
  if (port < 1024 || port > 65535) {
    printf("port number should be between 1024 ~ 65535.\n");
    exit(-1);
  }

  // implement your own code
}