#!/bin/bash

clang --target=wasm32 --no-standard-libraries -Wl,--no-entry -Wl,--export-all,-error-limit=0 -o ulisp-wasm.wasm ulisp-wasm.c
