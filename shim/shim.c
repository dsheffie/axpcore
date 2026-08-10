/* freestanding runtime shim for csmith-on-RTL : mini printf + string
 * routines + htif exit.  everything routes through the call_pal 0xb0
 * monitor convention. */
#include <stdint.h>
#include <stdarg.h>
#include <stddef.h>

volatile uint64_t tohost __attribute__((aligned(64)));
volatile uint64_t fromhost __attribute__((aligned(64)));

uint64_t stack[4096] __attribute__((aligned(16)));
asm(".globl stack_end\nstack_end = stack+32768");

static volatile uint64_t htif_buf[8] __attribute__((aligned(64)));

static void htif_write(const char *buf, uint64_t len) {
  htif_buf[0] = 64;
  htif_buf[1] = 1;
  htif_buf[2] = (uint64_t)buf;
  htif_buf[3] = len;
  tohost = (uint64_t)htif_buf;
  asm volatile("call_pal 0xb0" ::: "memory");
}

void exit(int code) {
  tohost = ((uint64_t)(uint32_t)code << 1) | 1;
  asm volatile("call_pal 0xb0" ::: "memory");
  for(;;);
}

size_t strlen(const char *s) {
  size_t n = 0;
  while(s[n]) {
    n++;
  }
  return n;
}

int strcmp(const char *a, const char *b) {
  while(*a && (*a == *b)) {
    a++;
    b++;
  }
  return (unsigned char)*a - (unsigned char)*b;
}

void *memcpy(void *d, const void *s, size_t n) {
  char *dp = d;
  const char *sp = s;
  for(size_t i = 0; i < n; i++) {
    dp[i] = sp[i];
  }
  return d;
}

void *memset(void *d, int c, size_t n) {
  char *dp = d;
  for(size_t i = 0; i < n; i++) {
    dp[i] = c;
  }
  return d;
}

static char *fmt_u64(char *p, uint64_t v, int base, int upper) {
  char tmp[24];
  int n = 0;
  const char *dig = upper ? "0123456789ABCDEF" : "0123456789abcdef";
  do {
    tmp[n++] = dig[v % base];
    v /= base;
  } while(v);
  while(n) {
    *p++ = tmp[--n];
  }
  return p;
}

/* just enough printf for csmith : %d %i %u %x %X %s %c %% with l/ll
 * modifiers.  output buffered and flushed once per call. */
int printf(const char *fmt, ...) {
  static char out[1024];
  char *p = out;
  va_list ap;
  va_start(ap, fmt);
  while(*fmt && (p < (out + sizeof(out) - 32))) {
    if(*fmt != '%') {
      *p++ = *fmt++;
      continue;
    }
    fmt++;
    int lcnt = 0;
    while(*fmt == 'l') {
      lcnt++;
      fmt++;
    }
    switch(*fmt)
      {
      case 'd':
      case 'i': {
	int64_t v = lcnt ? va_arg(ap, int64_t) : va_arg(ap, int32_t);
	if(v < 0) {
	  *p++ = '-';
	  v = -v;
	}
	p = fmt_u64(p, v, 10, 0);
	break;
      }
      case 'u': {
	uint64_t v = lcnt ? va_arg(ap, uint64_t) : va_arg(ap, uint32_t);
	p = fmt_u64(p, v, 10, 0);
	break;
      }
      case 'x': {
	uint64_t v = lcnt ? va_arg(ap, uint64_t) : va_arg(ap, uint32_t);
	p = fmt_u64(p, v, 16, 0);
	break;
      }
      case 'X': {
	uint64_t v = lcnt ? va_arg(ap, uint64_t) : va_arg(ap, uint32_t);
	p = fmt_u64(p, v, 16, 1);
	break;
      }
      case 's': {
	const char *s = va_arg(ap, const char*);
	while(*s && (p < (out + sizeof(out) - 2))) {
	  *p++ = *s++;
	}
	break;
      }
      case 'c': {
	*p++ = (char)va_arg(ap, int);
	break;
      }
      case '%': {
	*p++ = '%';
	break;
      }
      default: {
	*p++ = '%';
	*p++ = *fmt;
	break;
      }
      }
    fmt++;
  }
  va_end(ap);
  htif_write(out, p - out);
  return p - out;
}

int puts(const char *s) {
  printf("%s\n", s);
  return 0;
}
