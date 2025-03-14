📌 과제 1: 소켓 프로그래밍 실습

본 과제는 리눅스의 Berkeley socket API를 사용하여 S-client와 S-server라는 간단한 네트워크 응용 프로그램을 만드는 것이다. TCP 소켓 API(socket(), bind(), listen(), connect(), accept(), write(), read() 등)를 사용한다. API 사용법은 Beej’s guide to network programming 참조.

🔖 A. S-client

역할

표준 입력(stdin)으로 메시지를 읽고, 이를 s-server에 전송한 후 서버의 응답을 표준 출력(stdout)에 출력하고 종료한다.

실행 예시

./s-client -p 1080 -s 147.256.38.7 < file-to-send.txt

세부 요구사항

입력 메시지 크기: 최대 10MB (초과 시 나머지 버림)

입력이 없으면(EOF만 입력) 오류 메시지 출력 후 종료

Request 형식

POST message SIMPLE/1.0\r\n
Host: [서버 도메인 이름]\r\n
Content-length: [메시지 바이트 수]\r\n
\r\n
[메시지 내용]

헤더 라인은 CRLF(\r\n)으로 끝남

첫 줄 외 헤더 필드명은 대소문자 무관

Host와 Content-length 헤더 필드는 순서 무관하지만 반드시 존재해야 함

응답 처리

정상 응답(200 OK): 본문만 stdout에 출력

SIMPLE/1.0 200 OK\r\n
Content-length: [byte-count]\r\n
\r\n
[메시지 본문]

오류 응답(400 Bad Request): 헤더 전체를 stdout에 출력

SIMPLE/1.0 400 Bad Request\r\n
\r\n

🔖 B. S-server

역할

TCP 연결을 기다리고, s-client의 요청 처리 후 응답 전송

요구사항

최소 5개 이상의 동시 연결 처리

요청 형식이 잘못되면 오류 응답(400 Bad Request) 전송

응답 형식

정상적인 요청 시:

SIMPLE/1.0 200 OK\r\n
Content-length: [byte-count]\r\n
\r\n
[message-sent-from-s-client]

요청 메시지와 동일한 길이의 메시지를 반환해야 함

요청 메시지가 잘못되었을 때 400 Bad Request 반환 후 연결 종료

필수 구현 사항

최소 5개 동시 연결 처리 필수

Content-length와 정확히 일치하는 데이터 전송 필수 (교착 상태 방지)

입력 메시지가 텍스트일 거라 가정하지 않음 (널 바이트 포함될 수 있음)

SIGPIPE 신호 무시 설정(signal(SIGPIPE, SIG_IGN)) 권장

🔖 동시 접속 처리 방법

다음 두 가지 방법 중 택 1:

Blocking 방식 (Fork 기반, 권장)

요청마다 프로세스 fork로 처리

Non-blocking 방식 (Event-driven)

소켓을 Non-blocking으로 설정하여 이벤트 기반 처리

다음 과제(2번 과제)에서 이 방식 채택 시 추가 점수 부여

최소 5개의 동시 접속 처리 필수이며, 데이터 송수신량은 Content-length와 반드시 정확히 일치해야 함.

🔖 제출물

제출할 파일:

- sclient.c
- sserver.c
- macro.h (제공된 파일, 수정 금지)
- Makefile (수정 금지)
- readme.pdf (설명서)

압축 방식:

{학번}_{영문이름}_assign1/
├── sclient.c
├── sserver.c
├── macro.h
├── Makefile
└── readme.pdf

파일명 예시: 202412345_HaechanKim_assign1.tar.gz

제공된 submit.sh 사용 가능:

$ ./submit.sh 202412345 HaechanKim

🔖 채점 기준

파일 구성 및 제출 형식: 5점

readme.pdf 충실성: 10점

코드 스타일 및 컴파일 경고 없음: 5점

프로그램 정상 작동: 70점

에러 처리 정상 작동: 10점

⚠️ Makefile로 컴파일 실패 시 0점 (간단한 누락은 감점 처리 후 채점 가능)

🔖 협업 규칙

다른 학생과 논의 가능, 코드 공유 금지

직접 코드 작성 필수

의문 사항은 TA 또는 교수에게 문의할 것

