#pragma once

#include <cstdarg>

#include "Config.h"

void init_logger();
void flush_logger();

int print_to_menu(const char *fmt, ...);
int vprint_to_menu(const char *fmt, va_list args);
