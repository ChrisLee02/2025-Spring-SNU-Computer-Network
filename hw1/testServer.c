/*
 * testServer.c
 *
 * 이 테스트 서버는 s-client(또는 s클라이언트 역할의 프로그램)가 보내는 요청을
 * 먼저 읽은 후, 커맨드라인 인자로 지정한 테스트 모드에 따라 응답 메시지를
 * 전송합니다.
 *
 * 사용법:
 *   ./testServer -p port -m mode [-b body]
 *
 *   mode:
 *     0: 올바른 응답 (표준 헤더 순서)
 *        "SIMPLE/1.0 200 OK\r\nContent-length: <len>\r\n\r\n<body>"
 *     1: 올바른 응답 (헤더에 여분의 공백 포함)
 *        "SIMPLE/1.0    200 OK\r\n   Content-length:    <len>\r\n\r\n<body>"
 *     2: 오류 응답 (400 Bad Request, 본문 없음)
 *        "SIMPLE/1.0 400 Bad Request\r\n\r\n"
 *     3: 잘못된 응답 (첫 줄에 문제가 있음)
 *        "WRONG/1.0 200 OK\r\nContent-length: <len>\r\n\r\n<body>"
 *
 * 연결이 수립되면 클라이언트의 요청을 읽고, 이를 표준 출력에 표시한 후 지정된
 * 응답을 보내고 종료합니다.
 */

#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define CRLF "\r\n"
#define MAX_BUF (1024 * 1024)

/* write_all: total 바이트를 fd에 쓰며, 부분 쓰기를 처리 */
ssize_t write_all(int fd, const char *buf, size_t total) {
  size_t bytes_written = 0;
  while (bytes_written < total) {
    ssize_t res = write(fd, buf + bytes_written, total - bytes_written);
    if (res < 0) return res;
    if (res == 0) break;
    bytes_written += res;
  }
  return bytes_written;
}

/* read_all: 최대 max_size 바이트까지 읽어 buf에 저장 */
ssize_t read_all(int fd, char *buf, size_t max_size) {
  size_t bytes_read = 0;
  while (bytes_read < max_size) {
    ssize_t res = read(fd, buf + bytes_read, max_size - bytes_read);
    if (res < 0) return res;
    if (res == 0) break;
    bytes_read += res;
  }
  return bytes_read;
}

int main(int argc, char **argv) {
  if (argc < 5) {
    fprintf(stderr, "Usage: %s -p port -m mode [-b body]\n", argv[0]);
    fprintf(stderr,
            "  mode: 0 (valid), 1 (valid extra spaces), 2 (error), 3 "
            "(malformed)\n");
    exit(EXIT_FAILURE);
  }

  int port = -1;
  int mode = -1;
  char *body = NULL;

  for (int i = 1; i < argc; i++) {
    if (strcmp(argv[i], "-p") == 0 && i + 1 < argc) {
      port = atoi(argv[++i]);
    } else if (strcmp(argv[i], "-m") == 0 && i + 1 < argc) {
      mode = atoi(argv[++i]);
    } else if (strcmp(argv[i], "-b") == 0 && i + 1 < argc) {
      body = argv[++i];
    }
  }

  if (port <= 0 || mode < 0 || mode > 3) {
    fprintf(stderr, "Invalid port or mode\n");
    exit(EXIT_FAILURE);
  }

  // 모드 0, 1, 3는 응답 본문이 필요하므로, 미지정 시 기본값 사용
  if ((mode == 0 || mode == 1 || mode == 3) && body == NULL) {
    body = "Default response";
  }

  int listenfd = socket(AF_INET, SOCK_STREAM, 0);
  if (listenfd < 0) {
    perror("socket");
    exit(EXIT_FAILURE);
  }

  int opt = 1;
  setsockopt(listenfd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

  struct sockaddr_in serv_addr;
  memset(&serv_addr, 0, sizeof(serv_addr));
  serv_addr.sin_family = AF_INET;
  serv_addr.sin_addr.s_addr = INADDR_ANY;
  serv_addr.sin_port = htons(port);

  if (bind(listenfd, (struct sockaddr *)&serv_addr, sizeof(serv_addr)) < 0) {
    perror("bind");
    close(listenfd);
    exit(EXIT_FAILURE);
  }

  if (listen(listenfd, 1) < 0) {
    perror("listen");
    close(listenfd);
    exit(EXIT_FAILURE);
  }

  printf("Test server running on port %d, mode %d\n", port, mode);

  struct sockaddr_in client_addr;
  socklen_t addrlen = sizeof(client_addr);
  int connfd = accept(listenfd, (struct sockaddr *)&client_addr, &addrlen);
  if (connfd < 0) {
    perror("accept");
    close(listenfd);
    exit(EXIT_FAILURE);
  }

  // 클라이언트의 요청 읽기
  char request[MAX_BUF];
  memset(request, 0, sizeof(request));
  ssize_t req_len = read_all(connfd, request, sizeof(request) - 1);
  if (req_len < 0) {
    perror("read_all");
    close(connfd);
    close(listenfd);
    exit(EXIT_FAILURE);
  }
  request[req_len] = '\0';
  printf("Received client request:\n%s\n", request);

  // 응답 메시지 구성
  char response[MAX_BUF];
  memset(response, 0, sizeof(response));
  int body_len = (mode == 2) ? 0 : strlen(body);

  switch (mode) {
    case 0:
      // Valid response, standard header.
      sprintf(response,
              "SIMPLE/1.0 200 OK\r\n"
              "Content-length: %d\r\n"
              "\r\n"
              "%s",
              body_len, body);
      break;
    case 1:
      // Valid response with extra spaces.
      sprintf(response,
              "SIMPLE/1.0    200 OK\r\n"
              "   Content-length:    %d\r\n"
              "\r\n"
              "%s",
              body_len, body);
      break;
    case 2:
      // Error response: 400 Bad Request, no body.
      sprintf(response,
              "SIMPLE/1.0 400 Bad Request\r\n"
              "\r\n");
      break;
    case 3:
      // Malformed response, incorrect first line.
      sprintf(response,
              "WRONG/1.0 200 OK\r\n"
              "Content-length: %d\r\n"
              "\r\n"
              "%s",
              body_len, body);
      break;
    default:
      fprintf(stderr, "Unknown mode\n");
      close(connfd);
      close(listenfd);
      exit(EXIT_FAILURE);
  }

  size_t resp_len = strlen(response);
  if (write_all(connfd, response, resp_len) < 0) {
    perror("write_all");
  }

  close(connfd);
  close(listenfd);
  printf("Response sent, exiting.\n");
  return 0;
}
