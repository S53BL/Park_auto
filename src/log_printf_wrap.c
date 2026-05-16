/*
 * Workaround for pioarduino platform-espressif32 55.03.38:
 * ld_flags includes -Wl,--wrap=log_printf but __wrap_log_printf is not
 * defined anywhere in the precompiled framework libs. This stub provides
 * a pass-through to log_printfv (the va_list variant defined in esp32-hal-uart.c).
 */
#include <stdarg.h>

extern int log_printfv(const char *format, va_list arg);

int __wrap_log_printf(const char *format, ...) {
    va_list arg;
    va_start(arg, format);
    int len = log_printfv(format, arg);
    va_end(arg);
    return len;
}
