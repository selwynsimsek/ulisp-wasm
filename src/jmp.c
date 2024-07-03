/*
 * Hackish way to implement setjmp/longjmp in pure MVP wasm using Binaryen's
 * Asyncify feature.
 *
 * Background: Setjmp/longjmp works in Emscripten (on the Web, in Node.js, in
 * wasmer, etc.) but it depends on JS support (or on the runtime to support
 * the special invoke/dynCall imports). This file shows how to implement
 * setjmp/longjmp in pure MVP wasm and without any special imports, instead
 * using Asyncify to modify the wasm to allow pause/resume and with a setjmp/
 * longjmp implementation that uses that.
 *
 *  https://kripken.github.io/blog/wasm/2019/07/16/asyncify.html
 *
 * Compile with something like
 *
 *   clang -O --target=wasm32 -c jmp.c -o jmp.o -Wno-incomplete-setjmp-declaration -Wno-incompatible-library-redeclaration -g
 *   wasm-ld --no-entry --export-all --allow-undefined jmp.o -o jmp.wasm
 *   wasm-opt jmp.wasm --asyncify -O -o jmp_async.wasm -g --pass-arg=asyncify-ignore-imports
 *
 * Run with something like 
 *
 *  wasmer run jmp_async.wasm
 *  wasmtime jmp_async.wasm
 *
 * Printing code using wasi is from the very useful
 *
 *  https://gist.github.com/s-macke/6dd78c78be46214d418454abb667a1ba
 *
 * by @s-macke - thanks!
 */

// No #includes, do everything ourselves

#define NULL 0

#define WASM_IMPORT(module, base) \
  __attribute__((__import_module__(#module), __import_name__(#base)))

#define NOINLINE __attribute__((noinline))

typedef unsigned short uint16_t;
typedef unsigned int uint32_t;
typedef long size_t;

// The Asyncify API

void asyncify_start_unwind(void* buf) WASM_IMPORT(asyncify, start_unwind);
void asyncify_stop_unwind() WASM_IMPORT(asyncify, stop_unwind);
void asyncify_start_rewind(void* buf) WASM_IMPORT(asyncify, start_rewind);
void asyncify_stop_rewind() WASM_IMPORT(asyncify, stop_rewind);

// Enough WASI API to print

typedef uint16_t __wasi_errno_t;
typedef uint32_t __wasi_fd_t;

struct __wasi_ciovec_t {
  const void* buf;
  size_t buf_len;
};

__wasi_errno_t __wasi_fd_write(
  __wasi_fd_t fd,
  const struct __wasi_ciovec_t* iovs,
  size_t iovs_len,
  size_t* nwritten
) WASM_IMPORT(wasi_unstable, fd_write) __attribute__((__warn_unused_result__));

// Enough libc to print

NOINLINE
size_t strlen(const char* str) {
  const char* s;
  for (s = str;* s; ++s);
  return s - str;
}

// sends string to stdout
NOINLINE
void print(const char* str) {
  const __wasi_fd_t stdout = 1;
  size_t nwritten;
  __wasi_errno_t error;
  struct __wasi_ciovec_t iovec;
  iovec.buf = str;
  iovec.buf_len = strlen(str);
  error =__wasi_fd_write(stdout, &iovec, 1, &nwritten);
}

NOINLINE
void puts(const char* str) {
  print(str);
  print("\n");
}

//
// The "upper runtime" using Asyncify: A weird impl of setjmp/longjmp
//

#define ASYNC_BUF_BUFFER_SIZE 1000

// An Asyncify buffer.
struct async_buf {
  void* top; // current top of the used part of the buffer
  void* end; // fixed end of the buffer
  void* unwound; // top of the buffer when full (unwound and ready to rewind)
  char buffer[ASYNC_BUF_BUFFER_SIZE];
};

NOINLINE
void async_buf_init(struct async_buf* buf) {
  buf->top = &buf->buffer[0];
  buf->end = &buf->buffer[ASYNC_BUF_BUFFER_SIZE];
}

NOINLINE
void async_buf_note_unwound(struct async_buf* buf) {
  buf->unwound = buf->top;
}

NOINLINE
void async_buf_rewind(struct async_buf* buf) {
  buf->top = buf->unwound;
}

// A setjmp/longjmp buffer.
struct jmp_buf {
  // A buffer for the setjmp. Unwound and rewound immediately, and can
  // be rewound a second time to get to the setjmp from the longjmp.
  struct async_buf setjmp_buf;
  // A buffer for the longjmp. Unwound once and never rewound.
  struct async_buf longjmp_buf;
  // The value to return.
  int value;
  // FIXME We assume this is initialized to zero.
  int state;
};

static struct jmp_buf* __active_jmp_buf = NULL;

NOINLINE
int setjmp(struct jmp_buf* buf) {
  if (buf->state == 0) {
    __active_jmp_buf = buf;
    async_buf_init(&buf->setjmp_buf);
    asyncify_start_unwind(&buf->setjmp_buf);
  } else {
    asyncify_stop_rewind();
    if (buf->state == 2) {
      // We returned from the longjmp, all done.
      __active_jmp_buf = NULL;
    }
  }
  buf->state++;
  return buf->value;
}

NOINLINE
void longjmp(struct jmp_buf* buf, int value) {
  buf->value = value;
  async_buf_init(&buf->longjmp_buf);
  asyncify_start_unwind(&buf->longjmp_buf);
  // TODO: handle local var changes by updating the setjmp_buf
}

//
// The "lower runtime": Starts everything, is unwound to, resumes, etc.
//

void user_program();

void _start() {
  // This has enough logic to handle one longjmp.
  while (1) {
    // Call into the program. This is either the first call, or a resume.
    user_program();
    if (!__active_jmp_buf) {
      // The program has run to the end.
      return;
    }
    // The program is still working, just the stack has unwound to here.
    asyncify_stop_unwind();
    if (__active_jmp_buf->state == 1) {
      // Setjmp unwound to here. Prepare to rewind it twice.
      async_buf_note_unwound(&__active_jmp_buf->setjmp_buf);
    } else if (__active_jmp_buf->state == 2) {
      // Longjmp unwound to here. Rewind to the setjmp.
      async_buf_rewind(&__active_jmp_buf->setjmp_buf);
    }
    asyncify_start_rewind(&__active_jmp_buf->setjmp_buf);
  }
}

//===================================================================
// Start of the user program itself.
//===================================================================

struct jmp_buf my_buf;

// An inner function.
NOINLINE
void inner() {
  puts("call longjmp");
  longjmp(&my_buf, 1);
}

// The main part of the program (avoid main() because of wasi).
NOINLINE
void user_program() {
  puts("start");
  if (!setjmp(&my_buf)) {
    puts("call inner");
    inner();
  } else {
    puts("back from longjmp");
  }
  puts("end");
}

//===================================================================
// End of the user program itself.
//===================================================================
