#include <arpa/inet.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>

#define BUFSIZE 4096

// 에러 발생 시 메시지를 출력하고 종료하는 함수
void error_exit(const char *msg) {
  perror(msg);
  exit(EXIT_FAILURE);
}

// send()를 이용해 모든 데이터를 보내는 함수
int send_all(int sockfd, const char *buf, size_t len) {
  size_t total = 0;
  while (total < len) {
    ssize_t sent = send(sockfd, buf + total, len - total, 0);
    if (sent < 0) {
      if (errno == EINTR) continue;
      return -1;
    }
    total += sent;
  }
  return 0;
}

// 소켓에서 응답을 읽어 표준 출력에 출력하는 함수.
// select()를 이용해 데이터 도착을 기다리고, 타임아웃 시 반복을 종료합니다.
int read_response(int sockfd) {
  char buffer[BUFSIZE];
  int n;
  fd_set readfds;
  struct timeval timeout;

  // 데이터가 더 이상 도착하지 않을 때까지 반복
  while (1) {
    FD_ZERO(&readfds);
    FD_SET(sockfd, &readfds);
    // 2초 타임아웃 설정
    timeout.tv_sec = 2;
    timeout.tv_usec = 0;
    int ret = select(sockfd + 1, &readfds, NULL, NULL, &timeout);
    if (ret < 0) {
      perror("select");
      return -1;
    } else if (ret == 0) {
      // 타임아웃이면 더 이상 읽을 데이터가 없다고 판단
      break;
    }
    n = recv(sockfd, buffer, sizeof(buffer) - 1, 0);
    if (n < 0) {
      perror("recv");
      return -1;
    } else if (n == 0) {
      // 서버가 연결을 닫은 경우
      printf("Connection closed by server.\n");
      return 0;
    }
    buffer[n] = '\0';
    printf("%s", buffer);
  }
  return 1;
}

int main(int argc, char *argv[]) {
  if (argc < 3) {
    fprintf(stderr, "Usage: %s <server_ip> <port>\n", argv[0]);
    exit(EXIT_FAILURE);
  }
  const char *server_ip = argv[1];
  int port = atoi(argv[2]);

  // 소켓 생성 및 서버 연결
  int sockfd = socket(AF_INET, SOCK_STREAM, 0);
  if (sockfd < 0) error_exit("socket");

  struct sockaddr_in server_addr;
  memset(&server_addr, 0, sizeof(server_addr));
  server_addr.sin_family = AF_INET;
  server_addr.sin_port = htons(port);
  if (inet_pton(AF_INET, server_ip, &server_addr.sin_addr) < 0)
    error_exit("inet_pton");

  if (connect(sockfd, (struct sockaddr *)&server_addr, sizeof(server_addr)) < 0)
    error_exit("connect");

  // 요청 문자열 정의
  const char *keep_alive_req =
      "GET /test HTTP/1.1\r\nHost: localhost\r\nConnection: keep-alive\r\n\r\n";
  const char *close_req =
      "GET /test HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n";

  // keep-alive 요청 5회 전송 및 응답 출력
  for (int i = 0; i < 5; i++) {
    printf("========== Sending keep-alive request %d ==========\n", i + 1);
    if (send_all(sockfd, keep_alive_req, strlen(keep_alive_req)) < 0)
      error_exit("send_all");

    printf("---------- Response ----------\n");
    int ret = read_response(sockfd);
    if (ret < 0) error_exit("read_response");
    printf("\n==============================\n\n");
  }

  // 마지막 close 요청 전송
  printf("========== Sending close request ==========\n");
  if (send_all(sockfd, close_req, strlen(close_req)) < 0)
    error_exit("send_all");

  printf("---------- Response ----------\n");
  int ret = read_response(sockfd);
  if (ret < 0) error_exit("read_response");

  // close 요청 후 서버가 연결을 종료했는지 확인
  char buffer[BUFSIZE];
  int n = recv(sockfd, buffer, sizeof(buffer) - 1, 0);
  if (n == 0) {
    printf("\nVerified: Connection closed by server.\n");
  } else if (n < 0) {
    perror("recv");
  } else {
    buffer[n] = '\0';
    printf("\nUnexpected data after close request:\n%s", buffer);
  }

  close(sockfd);
  return 0;
}
