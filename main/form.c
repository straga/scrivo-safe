#include "form.h"

static int hex_value(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

bool form_decode(char *s)
{
    char *out = s;
    for (char *in = s; *in; in++) {
        if (*in == '+') {
            *out++ = ' ';
        } else if (*in == '%') {
            int hi = hex_value(in[1]);
            int lo = hi < 0 ? -1 : hex_value(in[2]);
            if (lo < 0) {
                return false;
            }
            *out++ = (char)(hi << 4 | lo);
            in += 2;
        } else {
            *out++ = *in;
        }
    }
    *out = '\0';
    return true;
}
