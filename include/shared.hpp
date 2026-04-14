#ifndef SHARED_H
#define SHARED_H

#define _POSIX_C_SOURCE 200112L
#include <stdio.h>
#include <unistd.h>
#include <ctype.h>
#include <errno.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netdb.h>
#include <string.h>
#include <pthread.h>
#include <fcntl.h>
#include <stdlib.h>
#include <stdarg.h>


// In C++
#include <iostream>
#include <vector>
#include <string>
#include <algorithm> // string lowercasing algo
#include <cctype>
#include <limits> // Required for std::numeric_limits

void private_log_logic(const char* func_name, int line, const char* format, ...);
#define LOG_ERROR(...) private_log_logic(__func__, __LINE__, __VA_ARGS__)


#define MAX_USERNAME_SIZE 32
#define MAX_PASSWORD_SIZE 32
#define MAX_MSG_CONTENT_SIZE 250 // MAX message size a user can type/send out

// -----------------------------------
// Database (File) States
// -----------------------------------

// Struct representing a user in our application.
typedef struct {
  uint32_t id;          // Incrementing ID associated with the user.
  std::string username; // Username of the user.
  std::string password; // Plain-text password for the user.
} user_t;

// -----------------------------------
// Connection Table States
// -----------------------------------

// Struct representing an existing TCP client
typedef struct {
  struct sockaddr_storage addr;  // An IPv4 or IPv6 address structure (128 bytes).
  user_t* user;                  // Pointer to the user associated with the connection; if pointer isn't NULL, this is an authenticated user
  int fd;                        // Integer containing the file descriptor for the TCP connection socket.
  bool want_read = false;        // Booleans indicating readiness intentions
  bool want_write = false;
  bool want_close = false;
  std::vector<uint8_t> incoming; // Buffer stores data read (and needs to be parsed) by our user-space app from the kernel.
  std::vector<uint8_t> outgoing; // Buffer stores  data that needs to be written to the peer.
} conn_t;

/**
 * Prompts input for a value within a given range.
 * 
 * @param prompt Input prompt we're going to repeat.
 * @param min Minimum valid value.
 * @param max Maximum valid value.
 * @return The valid integer value that they inputted.
 */
int get_valid_input_range(char *prompt, int min, int max);

/**
 * Gets keyboard input from stdin.
 * 
 * @param prompt Input prompt to be printed out.
 * @param buffer Buffer that we'll put the input in.
 * @param buf_size Maximum amount of bytes allocated for the buffer. 
 * We'll read up to buf_size - 1, and the last will be null terminator.
 * 
 * NOTE: 
 * - Safer than scanf as it protects from buffer overflow.
 * - Using fgets, if the user types more than the buffer, extra characters will stay in the stdin buffer 
 *   waiting for our next fgets() call. This function will clear those extra characters automatically (buffer flushing).
 * - If this function encounters errors, the program is immediately aborted. 
 */
void get_stdin(char *prompt, char *buffer, int buf_size);

/**
 * Lowercases an existing string and returns the lowercased version.
 * @param s String to be lowercased.
 * @return The lowercased version of the string.
 */
std::string string_to_lower(std::string s);

/**
 * Prints warning or system event messages to stderr.
 * It doesn't kill the application
 */
void msg(const char *msg);

/**
 * Shows an error message and kills the application.
 */
void die(const char *msg);

/**
 * Gets string input in a safe and robust manner
 * @param prompt Input prompt to print before prompting user for input.
 * @param buffer Reference to string buffer that we're going to write input into.
 */
void get_string_cin(std::string prompt, std::string& buffer);


#endif