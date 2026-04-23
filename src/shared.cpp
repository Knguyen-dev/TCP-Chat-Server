#include "shared.hpp"

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

void log_message(const char *prefix, const char *msg) {
    FILE *f = fopen("chat_server.log", "a");
    if (f == NULL) return;
    time_t now;
    time(&now);
    char *date = ctime(&now);
    date[strlen(date) - 1] = '\0'; // Remove newline
    fprintf(f, "[%s] %s: %s\n", date, prefix, msg);
    fclose(f);
}

int get_valid_input_range(std::string_view prompt, int min, int max) {
  int value;
  int status;
  while (1) {
    std::cout << prompt;
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

std::string string_to_lower(std::string s) {
  std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) {
    return std::tolower(c);
  });
  return s;
}

void msg(const char *msg) {
  fprintf(stderr, "%s\n", msg);
}

void die(const char *msg) {
  fprintf(stderr, "[%d] %s\n", errno, msg);
  abort();
}


void ignore_line() {
  std::cin.ignore(std::numeric_limits<std::streamsize>::max(), '\n');
}

void get_string_cin(std::string_view prompt, std::string& buffer) {
  while (true) {
    std::cout << prompt;
    std::cin >> buffer;
    if (!std::cin) {         // If prev extraction failed  
      if (!std::cin.eof()) { // If user entered an eof, then shut down
        std::exit(-1);
      }
      std::cin.clear(); // put std::cin back into 'normal' mode
      ignore_line();    // remove bad input from input stream      
      continue;
    }
    ignore_line(); // ignore any potential/additional input on the line that's invalid
    break;
  }
}