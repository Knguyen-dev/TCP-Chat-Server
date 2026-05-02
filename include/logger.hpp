#ifndef LOGGER_HPP
#define LOGGER_HPP

#include <cstdio>
#include <cstdarg>

// Enum representing the log level severity
enum LogLevel {
  LOG_DEBUG = 0,
  LOG_INFO = 1,
  LOG_WARN = 2,
  LOG_ERROR = 3,
};

/**
 * Log a message with automatic file:line info.
 * @param level Log level
 * @param file Source file name (__FILE__)
 * @param line Source line number (__LINE__)
 * @param format Printf-style format string
 * 
 * NOTE: This shouldn't be used directly. Please use the macro functions that are 
 * defined.
 */
void logger_log(LogLevel level, const char* file, int line, const char* format, ...);

#define LOG_DEBUG(...) logger_log(LOG_DEBUG, __FILE__, __LINE__, __VA_ARGS__)
#define LOG_INFO(...)  logger_log(LOG_INFO, __FILE__, __LINE__, __VA_ARGS__)
#define LOG_WARN(...)  logger_log(LOG_WARN, __FILE__, __LINE__, __VA_ARGS__)
#define LOG_ERROR(...) logger_log(LOG_ERROR, __FILE__, __LINE__, __VA_ARGS__)

#endif // LOGGER_HPP
