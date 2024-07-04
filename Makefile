CC = "${WASI_SDK_PATH}/bin/clang"
LD = "${WASI_SDK_PATH}/bin/wasm-ld"

WASI_STUB = ../wasi-stub/run.sh

SANDBOX ?= 1

build/ulisp.wasm: build/ulisp-wasm-opt.wasm
	ln -sf ulisp-wasm-opt.wasm build/ulisp.wasm
	wasm2wat build/ulisp.wasm > build/ulisp.wat

build/ulisp-wasm.o: src/ulisp-wasm.c
	$(CC) -D SANDBOX=$(SANDBOX)  --sysroot=${WASI_SDK_PATH}/share/wasi-sysroot -O -c src/ulisp-wasm.c --target=wasm32-wasi -nodefaultlibs -o build/ulisp-wasm.o

build/ulisp-wasm-preopt.wasm: build/ulisp-wasm.o
ifeq ($(SANDBOX), 0)
	$(LD) -L${WASI_SDK_PATH}/share/wasi-sysroot/lib/wasm32-wasi -lc --export-all build/ulisp-wasm.o -o build/ulisp-wasm-preopt.wasm
else
	$(LD) -L${WASI_SDK_PATH}/share/wasi-sysroot/lib/wasm32-wasi -lc --export-all --no-entry build/ulisp-wasm.o -o build/ulisp-wasm-preopt.wasm
endif
ifeq ($(SANDBOX), 1)
	$(WASI_STUB) build/ulisp-wasm-preopt.wasm
endif

build/ulisp-wasm-opt.wasm: build/ulisp-wasm-preopt.wasm
	wasm-opt -O4 build/ulisp-wasm-preopt.wasm -o build/ulisp-wasm-opt.wasm

clean:
	rm -rf build/*

run: build/ulisp.wasm
	wasmer run --dir=. -v -e init build/ulisp.wasm
