#ifndef MYSHELL_H
#define MYSHELL_H


#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define MAX_ARGS 128
#define PROMPT "shell> "


/**
 * Функции утилит
 */
void calculate_timespec_diff(const struct timespec* start, const struct timespec* end, struct timespec* result);
char** parse_command_line(char* line, int* argc_out);
int execute_command(char** argv);
void execute_with_and(char* line);

#endif