/* The platform calls the converters make, for the host: logging, the fatal-error exits and the disc reads.
 * (Allocation is the C library's calloc and free.) Generic: nothing here is specific to a game. */
#include <setjmp.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "host_decode.h"
#include "plat/disc.h"
#include "plat/plat.h"

static int s_level = PLAT_LOG_ERROR;
static jmp_buf* s_guard;
static char* s_msg;
static size_t s_msg_n;

void host_set_log_level(int level) { s_level = level; }

int plat_log_level(void) { return s_level; }

void plat_vlog(int level, const char* fmt, va_list ap) {
    if (level > s_level) {
        return;
    }
    static const char tag[] = {'E', 'W', 'I', 'D'};
    fprintf(stderr, "[%c] ", tag[level & 3]);
    vfprintf(stderr, fmt, ap);
    fputc('\n', stderr);
}

void plat_log(int level, const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    plat_vlog(level, fmt, ap);
    va_end(ap);
}

static void stop(const char* fmt, va_list ap) __attribute__((noreturn));
static void stop(const char* fmt, va_list ap) {
    if (s_guard == NULL) {
        fputs("fatal: ", stderr);
        vfprintf(stderr, fmt, ap);
        fputc('\n', stderr);
        exit(2);
    }
    vsnprintf(s_msg, s_msg_n, fmt, ap);
    longjmp(*s_guard, 1);
}

void plat_fatal(const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    stop(fmt, ap);
}

void plat_user_error(const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    stop(fmt, ap);
}

/* OSPanic(file, line, format, ...): the decomp's assertion exit. */
void OSPanic(const char* file, int line, const char* msg, ...) {
    char text[512];
    va_list ap;
    va_start(ap, msg);
    vsnprintf(text, sizeof(text), msg, ap);
    va_end(ap);
    plat_fatal("%s:%d: %s", file, line, text);
}

int host_guard_run(HostGuardFn fn, void* arg, char* msg, size_t msg_n) {
    jmp_buf here;
    jmp_buf* outer = s_guard;
    char* outer_msg = s_msg;
    size_t outer_n = s_msg_n;
    s_msg = msg;
    s_msg_n = msg_n;
    msg[0] = '\0';
    s_guard = &here;
    int stopped = setjmp(here);
    if (!stopped) {
        fn(arg);
    }
    s_guard = outer;
    s_msg = outer_msg;
    s_msg_n = outer_n;
    return stopped;
}

/* ---- the disc image (plat/disc.h, whose 3DS implementation talks to the SD card) ---- */

static FILE* s_disc;

int host_disc_attach(FILE* f) {
    s_disc = f;
    return 0;
}

int plat_disc_open(uint64_t* size_out) {
    if (s_disc == NULL) {
        return -1;
    }
    if (size_out != NULL) {
        fseeko(s_disc, 0, SEEK_END);
        *size_out = (uint64_t)ftello(s_disc);
    }
    return 0;
}

int32_t plat_disc_read(uint64_t offset, void* buf, uint32_t size) {
    if (s_disc == NULL) {
        return -1;
    }
    if (fseeko(s_disc, (off_t)offset, SEEK_SET) != 0) {
        return -2;
    }
    return (int32_t)fread(buf, 1, size, s_disc);
}

int32_t host_disc_read(uint64_t offset, void* buf, uint32_t n) { return plat_disc_read(offset, buf, n); }
