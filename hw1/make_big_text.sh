#!/bin/bash

FILE_SIZE=10485760  # 10MB = 10 * 1024 * 1024 바이트
OUTPUT_FILE="10mb.txt"

# /dev/zero에서 FILE_SIZE 만큼의 0 데이터를 읽어와서, tr로 'a'로 변환 후 파일에 저장
dd if=/dev/zero bs=$FILE_SIZE count=1 2>/dev/null | tr '\0' 'a' > "$OUTPUT_FILE"

echo "파일 '$OUTPUT_FILE' 생성 완료 (크기: ${FILE_SIZE}바이트)"
