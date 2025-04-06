#include <arpa/inet.h>
#include <errno.h>
#include <netdb.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define BUF_SIZE 8192

void send_request(const char* host, int port, const char* raw_request) {
  int sockfd;
  struct sockaddr_in server_addr;
  char buffer[BUF_SIZE];

  // 1. 소켓 생성
  if ((sockfd = socket(AF_INET, SOCK_STREAM, 0)) < 0) {
    perror("socket");
    return;
  }

  // 2. 주소 설정
  memset(&server_addr, 0, sizeof(server_addr));
  server_addr.sin_family = AF_INET;
  server_addr.sin_port = htons(port);
  if (inet_pton(AF_INET, host, &server_addr.sin_addr) < 0) {
    perror("inet_pton");
    close(sockfd);
    return;
  }

  // 3. 연결
  if (connect(sockfd, (struct sockaddr*)&server_addr, sizeof(server_addr)) <
      0) {
    perror("connect");
    close(sockfd);
    return;
  }

  // 4. 요청 전송
  write(sockfd, raw_request, strlen(raw_request));

  // 5. 응답 수신
  ssize_t n = read(sockfd, buffer, BUF_SIZE - 1);
  if (n < 0) {
    perror("read");
    close(sockfd);
    return;
  }
  buffer[n] = '\0';

  // 6. 상태코드 파싱
  printf("%s", buffer);

  close(sockfd);
}

int main(int argc, char* argv[]) {
  if (argc < 3) {
    printf("Usage: %s <host> <port>\n", argv[0]);
    exit(1);
  }

  const char* host = argv[1];
  int port = atoi(argv[2]);

  // ----------- 테스트 케이스 ----------
  const char* requests[] = {
      // valid 1.0
      "GET /test HTTP/1.0\r\nHost: test\r\n\r\n",
      "GET /test HTTP/1.1\r\n\r\n",
      // valid 1.1
      "GET /test HTTP/1.1\r\nHost: test\r\n\r\n",
      // 1.0 keep-alive
      "GET /test HTTP/1.0\r\nHost: test\r\nConnection: keep-alive\r\n\r\n",
      // 1.1 close
      "GET /test HTTP/1.1\r\nHost: test\r\nConnection: close\r\n\r\n",

      // malformed method
      "POST /test HTTP/1.0\r\nHost: test\r\n\r\n",
      // no host header (HTTP/1.1 requires it)
      // weird URI
      "GET http://localhost/ HTTP/1.1\r\nHost: test\r\n\r\n",
      // file not exist
      "GET /does_not_exist.html HTTP/1.1\r\nHost: test\r\n\r\n",
      // permission denied (if exists)
      "GET /denied.html HTTP/1.1\r\nHost: test\r\n\r\n",
  };

  int num = sizeof(requests) / sizeof(requests[0]);

  for (int i = 0; i < num; i++) {
    printf("\n==[Test %d]==\n", i + 1);
    send_request(host, port, requests[i]);
  }

  return 0;
}
