#!/bin/sh
#./makelib.sh
#g++ tfstools.cpp -L. -ltfstools -o tfstools -g
g++ tfstools.cpp gptpart.c fat32.c -o tfstools -g
