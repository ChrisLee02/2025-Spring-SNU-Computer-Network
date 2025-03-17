📌 과제 1: 소켓 프로그래밍 실습 (Socket programming practice)

본 프로젝트에서는 Linux의 표준 Berkeley socket API를 이용하여 간단한 네트워크 응용 프로그램 한 쌍(S-client, S-server)을 구현한다. TCP 소켓 API(socket(), bind(), listen(), connect(), accept(), write(), read() 등)를 사용하며, 자세한 API 사용법은 Beej’s guide to network programming을 참조할 것.

🔖 A. S-client

역할: 표준 입력(stdin)으로부터 메시지를 받아 s-server에 요청을 보내고, 응답을 표준 출력(stdout)에 출력한 후 종료한다.

실행 방법 예시

./s-client -p 1080 -s 147.256.38.7 < file-to-send.txt

세부 요구사항

입력 메시지 크기: 최대 10 MB (초과 시 초과분 버림)

입력 메시지가 없을 경우(EOF만 입력): 오류 메시지 출력 후 종료

요청(request) 형식

POST message SIMPLE/1.0\r\n
Host: [서버 도메인 이름]\r\n
Content-length: [메시지 바이트 수]\r\n
\r\n
[메시지 내용]

모든 헤더 라인은 CRLF (\r\n)으로 끝남

첫 줄을 제외한 헤더 필드명(Host, Content-length)은 대소문자 구분하지 않음

토큰 사이에 임의의 공백 허용

Host와 Content-length 헤더 필드는 순서 무관하게 둘 다 있어야 함

응답(response) 처리

정상 응답: 본문(message-content)만 stdout에 출력

SIMPLE/1.0 200 OK\r\n
Content-length: [byte-count]\r\n
\r\n
[message-content]

오류 응답: 헤더 전체를 stdout에 출력

SIMPLE/1.0 400 Bad Request\r\n
\r\n

🔖 B. S-server

역할: TCP 연결을 기다리고, s-client의 요청을 처리하여 응답을 전송한다.

동작 요구사항

하나 이상의 s-client로부터 요청 수신 및 처리

최소 5개 이상의 동시 연결 처리 지원 필수

요청 형식이 올바르지 않을 경우 오류 응답(400 Bad Request) 반환

동시 접속 처리 방식

아래 두 가지 중 하나 선택 가능

Blocking 방식 (권장)

요청마다 별도의 프로세스를 생성하여 처리 (fork 기반)

Non-blocking 방식 (Event-driven)

소켓을 non-blocking으로 설정하여 이벤트 기반 처리

과제 2에서 이 방식 채택 시 추가 점수 부여

필수 사항

최소 5개 동시 접속 처리 가능해야 함

데이터 송수신량은 Content-length와 정확히 일치해야 함 (교착상태 방지)

입력 메시지가 텍스트라는 가정 X

SIGPIPE 신호 무시 설정 권장 (signal(SIGPIPE, SIG_IGN))

🔖 제출물

- sclient.c
- sserver.c
- macro.h (수정 금지)
- Makefile (수정 금지)
- readme.pdf (설명서)

파일 압축 방식:

{학번}_{영문이름}_assign1/
├── sclient.c
├── sserver.c
├── macro.h
├── Makefile
└── readme.pdf

압축파일 예시: 202412345_HaechanKim_assign1.tar.gz

제공된 submit.sh 스크립트 사용 가능

🔖 채점 기준

파일 구조 및 제출 형식 준수: 5점

readme.pdf 충실성: 10점

코드 스타일 및 컴파일 경고 없음: 5점

프로그램 기능 정상 작동: 70점

에러 처리 기능 정상 작동: 10점

※ 제공된 Makefile로 컴파일이 실패하면 0점 (단, 간단한 누락 헤더 등은  수정하여 평가 가능하며 이 경우 0점 처리 대신 감점 가능)

🔖 협업 규칙

다른 학생과 논의 가능, 하지만 코드 공유 금지

자신의 코드 직접 작성 필수

의문 사항은 TA 또는 교수에게 문의 권장

