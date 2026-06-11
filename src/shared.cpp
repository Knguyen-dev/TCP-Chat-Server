#include "shared.hpp"


static uint8_t to_utype(ConnFlags f) {
  return static_cast<uint8_t>(f);
}

ConnFlags operator|(ConnFlags lhs, ConnFlags rhs) {
  return static_cast<ConnFlags>(to_utype(lhs) | to_utype(rhs));
}

ConnFlags operator&(ConnFlags lhs, ConnFlags rhs) {
  return static_cast<ConnFlags>(to_utype(lhs) & to_utype(rhs));
}

ConnFlags operator~(ConnFlags flag) {
  return static_cast<ConnFlags>(~to_utype(flag));
}

ConnFlags operator|=(ConnFlags& lhs, ConnFlags rhs) {
  lhs = lhs | rhs;
  return lhs;
}

ConnFlags operator&=(ConnFlags& lhs, ConnFlags rhs) {
  lhs = lhs & rhs;
  return lhs;
}

// Helper for checking flags: returns true if the flag is set
bool has_flag(ConnFlags mask, ConnFlags flag) {
    return (static_cast<uint8_t>(mask) & static_cast<uint8_t>(flag)) != 0;
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