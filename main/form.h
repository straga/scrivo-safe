// Decoding of application/x-www-form-urlencoded values and URL query values.
#pragma once

#include <stdbool.h>

// Decodes %XX escapes and '+' in place; false on a broken %XX escape.
bool form_decode(char *s);
