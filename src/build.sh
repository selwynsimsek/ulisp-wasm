#!/bin/bash

CC="${WASI_SDK_PATH}/bin/clang --sysroot=${WASI_SDK_PATH}/share/wasi-sysroot"

$CC -Wl,--no-entry -Wl,--export-all -o ulisp-wasm.wasm ulisp-wasm.c

wasi_stub=../../wasi-stub/run.sh

$wasi_stub ulisp-wasm.wasm

wasm2wat ulisp-wasm.wasm > ulisp-wasm.wat
