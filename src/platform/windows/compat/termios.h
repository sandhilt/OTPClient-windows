#pragma once
#include <windows.h>
struct termios { DWORD c_lflag; };
#define ECHO ENABLE_ECHO_INPUT
#define ECHOE 0
#define ECHOK 0
#define ECHONL 0
#define TCSAFLUSH 0
int tcgetattr (int fd, struct termios *state);
int tcsetattr (int fd, int action, const struct termios *state);
