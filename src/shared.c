#include "shared.h"

/**
 * Reusable error logging function
 * @param func_name The function the error happened at 
 * @param line The line number the error happened at.
 * @param msg The message that the developer put to describe the error.
 */
void private_log_logic(const char* func_name, int line, const char* format, ...) {
    // 1. Print the header (Function and Line)
    // 2. Initialize the variable argument list
    // 3. Use vfprintf to print the formatted string to stderr
    // 4. Clean up and add a newline
    fprintf(stderr, "[ERROR] %s:%d - ", func_name, line);
    
    va_list args;
    va_start(args, format);

    vfprintf(stderr, format, args);

    va_end(args);
    fprintf(stderr, "\n");
}

int get_valid_input_range(char *prompt, int min, int max) {
  int value;
  int status;
  while (1) {
    printf("%s", prompt);
    status = scanf("%d", &value);
    
    // 1. Handle non-numeric input
    // NOTE: Clears newline character from the buffer or until eof
    if (status != 1) {
      printf("Non-numeric input! Enter a NUMBER in range[%d, %d]: ", min, max);
      while (getchar() != '\n'); 
      continue;
    }

    // 2. Handle numeric range validation
    if (value < min || value > max) {
      printf("Out of range, enter a number in range[%d, %d]: ", min, max);
    } else {
      return value;
    }
  }
}

void get_stdin(char *prompt, char *buffer, int buf_size) {
  printf("%s", prompt);
  if (fgets(buffer, buf_size, stdin)) {
    char *newline = strchr(buffer, '\n');
    if (newline) {
      *newline = '\0';
    } else {
      int c;
      while ((c = getchar()) != '\n' && c != EOF);
    }
  } else {
    fprintf(stderr, "fgets() Failed: Error reading password! Terminating process!\n");
    exit(-1);
  }
}