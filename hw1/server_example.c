#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#define PORT 1111
#define MAX_HDR 1024
#define MAX_CONT (10 * 1024)
#define CRLFCRLF "\r\n\r\n"

/* write all */
ssize_t write_all(int fd, const void* buf, size_t total) {
  size_t bytes_written = 0;
  while (bytes_written < total) {
    ssize_t res =
        write(fd, (const char*)buf + bytes_written, total - bytes_written);
    if (res < 0) {
      return res;
    } else if (res == 0) {
      break;
    }
    bytes_written += res;
  }
  return total;
}

/* read all */
ssize_t read_all(int fd, void* buf, size_t max_read_size) {
  size_t bytes_read = 0;
  while (bytes_read < max_read_size) {
    ssize_t res = read(fd, (char*)buf + bytes_read, max_read_size - bytes_read);
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

int main() {
  int server_fd, client_fd;
  struct sockaddr_in address;
  socklen_t addrlen = sizeof(address);

  char buffer[MAX_HDR + MAX_CONT + 10];

  /* 소켓 생성 */
  if ((server_fd = socket(AF_INET, SOCK_STREAM, 0)) == 0) {
    fprintf(stderr, "bad response: no CRLFCRLF\n");
    exit(EXIT_FAILURE);
  }

  /* 주소 설정 */
  address.sin_family = AF_INET;
  address.sin_addr.s_addr = INADDR_ANY;
  address.sin_port = htons(PORT);

  /* 소켓 바인딩 */
  if (bind(server_fd, (struct sockaddr*)&address, sizeof(address)) < 0) {
    perror("bind failed");
    exit(EXIT_FAILURE);
  }

  /* 클라이언트 연결 대기 */
  if (listen(server_fd, 5) < 0) {
    perror("listen failed");
    exit(EXIT_FAILURE);
  }

  printf("Server is running on port %d...\n", PORT);

  /* 클라이언트 연결 대기 */
  while (1) {
    printf("Waiting for a connection...\n");
    if ((client_fd = accept(server_fd, (struct sockaddr*)&address, &addrlen)) <
        0) {
      perror("accept failed");
      continue;
    }
    printf("Client connected!\n");

    /* 요청 읽기 */
    ssize_t req_size =
        read_all(client_fd, buffer + req_size, MAX_HDR + MAX_CONT - req_size);

    buffer[req_size] = '\0';  // 문자열 종료 (디버깅용)

    /* 요청 출력 */
    printf("Received Request:\n%s\n", buffer);

    /* 400 Bad Request 응답 전송 */
    /* const char* response =
        "SIMPLE/1.0 400 Bad Request\r\n"
        "\r\n"; */

    /* 200 ok 예시응답 전송 */
    const char* response =
        "SIMPLE/1.0 200 OK\r\n"
        "Content-length: 6\r\n"
        "\r\n"
        "Hello\n";

    write_all(client_fd, response, strlen(response));
    close(client_fd);
    printf("Sent Response: 400 Bad Request\n");

    /* 연결 종료 */
    close(client_fd);
  }

  return 0;
}
