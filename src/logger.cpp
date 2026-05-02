#include "logger.hpp"
#include <cstring>

const char* level_to_string(LogLevel level) {
  switch (level) {
    case LOG_DEBUG: return "DEBUG";
    case LOG_INFO:  return "INFO";
    case LOG_WARN:  return "WARN";
    case LOG_ERROR: return "ERROR";
    default:        return "UNKNOWN LOG LEVEL";
  }
}

void logger_log(LogLevel level, const char* file, int line, const char* format, ...) {
  // Extract just the filename, not the full path
  const char* filename = strrchr(file, '/');
  filename = filename ? filename + 1 : file;

  // Format the message
  char buffer[1024];
  va_list args;
  va_start(args, format);
  vsnprintf(buffer, sizeof(buffer), format, args);
  va_end(args);

  // Log to stderr
  fprintf(stderr, "[%s] %s:%d - %s", level_to_string(level), filename, line, buffer);
}
