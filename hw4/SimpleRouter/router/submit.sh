#!/bin/bash

ID=202118641
NAME=HadongLee
DIRNAME="${ID}_${NAME}_assign4"
TARNAME="${ID}_assign4.tar.gz"

# 1. 폴더 생성
mkdir -p "$DIRNAME"

# 2. 파일 복사 (수정: 필요한 파일명은 직접 추가!)
cp sr_router.c sr_arpcache.c readme.pdf "$DIRNAME"

# 3. 압축
tar zcf "$TARNAME" "$DIRNAME"

echo "압축 파일이 생성되었습니다: $TARNAME"
