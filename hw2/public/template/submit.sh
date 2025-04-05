#!/bin/bash

if [ ! $# -eq 2 ]; then
    echo "usage: $0 [student_id] [name]"
    exit
fi

ID=$1
NAME=$2
echo "student id: $ID"
echo "student name: $NAME"

FOLDER="${ID}_${NAME}_assign2"
if [ ! -e $FOLDER ]; then
    mkdir $FOLDER
else
    echo "$FOLDER already exist!"
fi

SHTTPD="shttpd.c"
MACRO="macro.h"
README="readme"
MAKEFILE="Makefile"

if [ ! -e $SHTTPD ]; then
    echo "$SHTTPD is missing!"
    exit
elif [ ! -e $MACRO ]; then
    echo "$MACRO is missing!"
    exit
elif [ ! -e $README ]; then
    echo "$README is missing!"
    exit
elif [ ! -e $MAKEFILE ]; then
    echo "$MAKEFILE is missing!"
    exit
fi

cp $SHTTPD $MACRO $README $MAKEFILE $FOLDER

OUTPUT="${FOLDER}.tar.gz"

if [ -e $OUTPUT ]; then
    echo "$OUTPUT already exist, delete old one."
    rm $OUTPUT
fi

tar zcf $OUTPUT $FOLDER