#ifndef SHARED_H
#define SHARED_H

#define _POSIX_C_SOURCE 200112L
#define LISTENQ 100
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

void private_log_logic(const char* func_name, int line, const char* format, ...);
#define LOG_ERROR(...) private_log_logic(__func__, __LINE__, __VA_ARGS__)


#define MAX_USERNAME_SIZE 32
#define MAX_PASSWORD_SIZE 32
#define MAX_MSG_CONTENT_SIZE 250 // MAX message size a user can type/send out

// -----------------------------------
// Database (File) States
// -----------------------------------

// Struct representing a user in our application.
// NOTE: 72 bytes, tail padding of 2 bytes.
typedef struct {
  uint32_t id;                          // Incrementing ID associated with the user.
  char username[MAX_USERNAME_SIZE + 1]; // Username of the user.
  char password[MAX_PASSWORD_SIZE + 1]; // Plain-text password for the user.
} user_t;

// -----------------------------------
// Connection Table States
// -----------------------------------

// Struct representing an existing TCP client
// NOTE: 144 bytes, 4 bytes of padding
typedef struct {
  struct sockaddr_storage addr; // An IPv4 or IPv6 address structure (128 bytes).
  user_t* user;                 // Pointer to the user associated with the connection; if pointer isn't NULL, this is an authenticated user
  int connfd;                   // Integer containing the file descriptor for the TCP connection socket.
} conn_t;

// Struct representing dynamic array of connections. 
// NOTE: Indexed by connfd, so accessing items[i] accesses conn_t associated with 
// connection descriptor integer i. In total, 24 bytes, no padding.
typedef struct {
  size_t count;    // Current number of items in the array
  size_t capacity; // Maximum number of entries in the dynamic array
  conn_t* items;   // Pointer to the start of an array of conn_t structs
} Connections;

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
 * Lowercases an existing string
 * @param str The input string we're trying to lowercase.
 */
void string_to_lower(char* str);

#endif