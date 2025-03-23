#!/bin/bash

if [ ! $# -eq 2 ]; then
    echo "usage: $0 [student_id] [name]"
    exit
fi

# remove dash
ID=$(echo "$1" | tr -d '-')
NAME=$2

echo "student id: $ID"
echo "student name: $NAME"

FOLDER="${ID}_${NAME}_assign1"
if [ ! -e "$FOLDER" ]; then
    mkdir "$FOLDER"
else
    echo "$FOLDER already exists!"
fi

SCLIENT="sclient.c"
SSERVER="sserver.c"
MACRO="macro.h"
README="readme.pdf"
MAKEFILE="Makefile"

MISSING_FILE=""
for FILE in $SCLIENT $SSERVER $MACRO $README $MAKEFILE; do
    if [ ! -e "$FILE" ]; then
        echo "$FILE is missing!"
        MISSING_FILE=1
    fi
done

if [ "$MISSING_FILE" ]; then
    exit
fi

cp $SCLIENT $SSERVER $MACRO $README $MAKEFILE "$FOLDER"

OUTPUT="${FOLDER}.tar.gz"

if [ -e "$OUTPUT" ]; then
    echo "$OUTPUT already exists, deleting old one."
    rm "$OUTPUT"
fi

tar zcf "$OUTPUT" "$FOLDER"

