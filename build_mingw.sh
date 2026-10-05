#!/bin/sh
# Run this from the MSYS2 UCRT64 terminal
windres Nada.rc -O coff -o Nada_res.o
g++ -std=c++17 -municode -mwindows -O2 Nada.cpp Nada_res.o -o Nada.exe -static -lole32 -lshell32 -lgdi32 -lcomctl32
