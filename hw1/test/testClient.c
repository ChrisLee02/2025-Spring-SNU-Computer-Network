/*
 * testClient.c
 *
 * 이 프로그램은 s-server를 대상으로 여러 케이스의 요청을 보내어
 * 헤더 파싱의 유연성과 에러 처리가 제대로 동작하는지 테스트합니다.
 *
 * 사용법: ./testClient -p port -s server-ip
 */

#include <arpa/inet.h>
#include <netdb.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define CRLF "\r\n"
#define MAX_BUF (1024 * 1024)

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

struct TestCase {
  const char *description;
  char request[MAX_BUF];
};

int main(int argc, char **argv) {
  if (argc < 5) {
    fprintf(stderr, "Usage: %s -p port -s server\n", argv[0]);
    exit(EXIT_FAILURE);
  }

  int port = -1;
  char *server = NULL;
  for (int i = 1; i < argc; i++) {
    if (strcmp(argv[i], "-p") == 0 && i + 1 < argc) {
      port = atoi(argv[++i]);
    } else if (strcmp(argv[i], "-s") == 0 && i + 1 < argc) {
      server = argv[++i];
    }
  }
  if (port <= 0 || server == NULL) {
    fprintf(stderr, "Usage: %s -p port -s server\n", argv[0]);
    exit(EXIT_FAILURE);
  }

  // 기본 본문 (요청 본문)
  const char *body = "Hello Test";
  int body_len = strlen(body);

// 여러 테스트 케이스 준비 (올바른 요청과 에러 요청 포함)
#define NUM_TESTS 6
  struct TestCase tests[NUM_TESTS];

  // Test 0: 올바른 요청, 표준 헤더 순서 (Host 후 Content-length)
  tests[0].description = "Valid request, standard header order";
  sprintf(tests[0].request,
          "POST message SIMPLE/1.0\r\n"
          "Host: localhost\r\n"
          "Content-length: %d\r\n"
          "\r\n"
          "%s",
          body_len, body);

  // Test 1: 올바른 요청, 헤더 순서를 뒤집음 (Content-length 후 Host)
  tests[1].description = "Valid request, reversed header order";
  sprintf(tests[1].request,
          "POST message SIMPLE/1.0\r\n"
          "Content-length: %d\r\n"
          "Host: localhost\r\n"
          "\r\n"
          "%s",
          body_len, body);

  // Test 2: 올바른 요청, 토큰 양쪽에 여분의 공백 포함
  tests[2].description = "Valid request with extra spaces";
  sprintf(tests[2].request,
          "POST    message    SIMPLE/1.0\r\n"
          "   Host:    localhost   \r\n"
          "   Content-length:    %d   \r\n"
          "\r\n"
          "%s",
          body_len, body);

  // Test 3: 잘못된 요청, Host 헤더 누락
  tests[3].description = "Invalid request: missing Host header";
  sprintf(tests[3].request,
          "POST message SIMPLE/1.0\r\n"
          "Content-length: %d\r\n"
          "\r\n"
          "%s",
          body_len, body);

  // Test 4: 잘못된 요청, Content-length 헤더 누락
  tests[4].description = "Invalid request: missing Content-length header";
  sprintf(tests[4].request,
          "POST message SIMPLE/1.0\r\n"
          "Host: localhost\r\n"
          "\r\n"
          "%s",
          body);

  // Test 5: 잘못된 요청, 선언된 Content-length보다 본문 길이가 짧음
  tests[5].description =
      "Invalid request: body length less than declared Content-length";
  sprintf(tests[5].request,
          "POST message SIMPLE/1.0\r\n"
          "Host: localhost\r\n"
          "Content-length: %d\r\n"
          "\r\n"
          "%s",
          body_len + 5, body);

  // 각 테스트 케이스마다 소켓 생성, 요청 전송, 응답 수신 후 출력
  for (int i = 0; i < NUM_TESTS; i++) {
    printf("Test %d: %s\n", i, tests[i].description);
    int sockfd = socket(AF_INET, SOCK_STREAM, 0);
    if (sockfd < 0) {
      perror("socket");
      continue;
    }
    struct hostent *hp = gethostbyname(server);
    if (hp == NULL) {
      fprintf(stderr, "gethostbyname() failed\n");
      close(sockfd);
      continue;
    }
    struct sockaddr_in serv_addr;
    memset(&serv_addr, 0, sizeof(serv_addr));
    serv_addr.sin_family = AF_INET;
    serv_addr.sin_port = htons(port);
    memcpy(&serv_addr.sin_addr, hp->h_addr_list[0], hp->h_length);

    if (connect(sockfd, (struct sockaddr *)&serv_addr, sizeof(serv_addr)) < 0) {
      perror("connect");
      close(sockfd);
      continue;
    }
    size_t req_len = strlen(tests[i].request);
    if (write_all(sockfd, tests[i].request, req_len) < 0) {
      perror("write_all");
      close(sockfd);
      continue;
    }
    shutdown(sockfd, SHUT_WR);
    char resp[MAX_BUF];
    ssize_t resp_len = read_all(sockfd, resp, sizeof(resp) - 1);
    if (resp_len < 0) {
      perror("read_all");
      close(sockfd);
      continue;
    }
    resp[resp_len] = '\0';
    printf("Response:\n%s\n", resp);
    close(sockfd);
    printf("-------------------------\n");
    sleep(1);  // 테스트 간 약간의 지연
  }
  return 0;
}
