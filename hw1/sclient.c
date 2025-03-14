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

  signal(SIGPIPE, SIG_IGN);

  /* logic 1: connect to host */

  int socketfd;
  struct hostent* hp;
  struct sockaddr_in saddr;
  char* buffer;
  char header[MAX_HDR + 10];

  if ((socketfd = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP)) < 0) {
    fprintf(stderr, "socket() failed\n");
    return EXIT_FAILURE;
  }

  if ((hp = gethostbyname(pserver)) == NULL) {
    fprintf(stderr, "gethostbyname() failed\n");
    close(socketfd);
    return EXIT_FAILURE;
  }

  saddr.sin_family = AF_INET;
  memcpy(&saddr.sin_addr.s_addr, hp->h_addr_list[0], hp->h_length);
  saddr.sin_port = htons(port);

  if (connect(socketfd, (struct sockaddr*)&saddr, sizeof(saddr)) < 0) {
    fprintf(stderr, "connect() failed\n");
    close(socketfd);
    return EXIT_FAILURE;
  }

  /* logic 2: Read from Standard input */

  ssize_t req_body_size, req_header_size, tot_write_size;

  buffer = malloc(MAX_HDR + MAX_CONT + 10);
  if (!buffer) {
    fprintf(stderr, "malloc failed\n");
    goto error;
  }

  req_body_size = read_all(STDIN_FILENO, buffer, MAX_CONT);

  if (req_body_size < 0) {
    fprintf(stderr, "read failed\n");
    goto error;
  }

  if (req_body_size == 0) {
    fprintf(stderr, "empty body\n");
    goto error;
  }

  /* logic 3: Format to simple HTTP */

  req_header_size = snprintf(header, MAX_HDR,
                             "POST message SIMPLE/1.0\r\n"
                             "Host: %s\r\n"
                             "Content-length: %ld\r\n"
                             "\r\n",
                             pserver, req_body_size);

  if (req_header_size < 0) {
    fprintf(stderr, "snprintf failed\n");
    goto error;
  }

  if (req_header_size > MAX_HDR) {
    fprintf(stderr, "too large header\n");
    goto error;
  }

  /* logic 4: send header */

  tot_write_size = write_all(socketfd, header, req_header_size);

  if (tot_write_size < 0) {
    fprintf(stderr, "write failed\n");
    goto error;
  } else if (tot_write_size < req_header_size) {
    fprintf(stderr, "1connection closed\n");
    goto error;
  }
  /* logic 5: send body */

  tot_write_size = write_all(socketfd, buffer, req_body_size);

  if (tot_write_size < 0) {
    fprintf(stderr, "write failed\n");
    goto error;
  } else if (tot_write_size < req_body_size) {
    fprintf(stderr, "2connection closed\n");
    goto error;
  }
  shutdown(socketfd, SHUT_WR);  // send EOF

  /* logic 6: receive header of response

      SIMPLE/1.0 200 OK\r\n
      Content-length: [byte-count]\r\n
      \r\n
      [message-content]

      or

    SIMPLE/1.0 400 Bad Request\r\n
      \r\n

    Can assume well-formed response
  */

  /* 의사코드 작성:
  1. read header + body, EOF까지 쭉 읽는다.
  2. 읽는 과정에서 CRLFCRLF를 만나면 header 위치를 설정.
  3. header를 다 읽은 후, content-length를 찾아서 body를 읽는다.
  4. body를 다 읽은 후, 출력한다.
  */

  ssize_t res_size;

  res_size = read_all(socketfd, buffer, MAX_HDR + MAX_CONT);

  if (res_size < 0) {
    fprintf(stderr, "2read failed\n");
    goto error;
  } else if (res_size == 0) {
    fprintf(stderr, "connection closed\n");
    goto error;
  }

  buffer[res_size] = '\0';  // add for using string functions

  char* header_end = strstr(buffer, CRLFCRLF);
  if (!header_end) {
    fprintf(stderr, "bad response: no CRLFCRLF\n");
    goto error;
  }

  /* 헤더 첫 줄이 200 인지 400 인지 확인  */

  char* body_start = header_end + 4;

  if (strncmp(buffer, "SIMPLE/1.0 200 OK\r\n", 19) == 0) {
    /* parse header, get content-length and write */
    write_all(STDOUT_FILENO, body_start, res_size - (body_start - buffer));
  } else {
    // print header
    write_all(STDOUT_FILENO, buffer, res_size);
  }

  /* Clean up */
  free(buffer);
  close(socketfd);
  return EXIT_SUCCESS;

error:
  free(buffer);
  close(socketfd);
  return EXIT_FAILURE;
}