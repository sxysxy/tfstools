#!/bin/sh
gcc gptpart.c fat32.c -fPIC --shared -o libtfstools.so
