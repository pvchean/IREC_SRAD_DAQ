/* Force-included in every unit of the host test: system headers and a complete DIR type (glibc leaves it open). */
#ifndef HOSTENV_BASE_H
#define HOSTENV_BASE_H
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <errno.h>
#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#include <time.h>
#include <stdint.h>
#include <stdbool.h>
#include <limits.h>
struct __dirstream { int dummy; };
#endif
