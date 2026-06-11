#ifndef SHARED_H
#define SHARED_H

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
#include <string_view>
#include <sstream> // string streams 

#define MAX_USERNAME_SIZE 32
#define MAX_PASSWORD_SIZE 32
#define MAX_MSG_CONTENT_SIZE 250 // MAX message size a user can type/send out


// ##### Client and Load Testing Structs #####
// NOTE: These are kind of legacy, and are kept so that the client 
// and the laod tester still works. The main focus is the TCP server!

/**
 * Struct fully representing a user in the database.
 * @note On the server side it's used in user registration and login, rather 
 * than actually being used to represent users in some kind of connection table.
 * The client side makes use of this as well.
 */
struct user_t {
    uint32_t user_id;
    std::string username;
    std::string password;
};

// Struct representing an existing TCP client
struct conn_t {
  struct sockaddr_storage addr;  // An IPv4 or IPv6 address structure (128 bytes).
  user_t* user = nullptr;                  // Pointer to the user associated with the connection; if pointer isn't NULL, this is an authenticated user
  int fd = -1;                        // Integer containing the file descriptor for the TCP connection socket.
  bool want_read = false;        // Booleans indicating readiness intentions
  bool want_write = false;
  bool want_close = false;
  std::vector<uint8_t> incoming; // Buffer stores data read (and needs to be parsed) by our user-space app from the kernel.
  std::vector<uint8_t> outgoing; // Buffer stores  data that needs to be written to the peer.
};


// ##### Connection flags and overloads #####

/**
 * ENUM representing the various connection flags and states that can be 
 * toggled on a TCP connection.
 * 
 * @note This has a base of uint8_t meaning that you can treat 
 * a ConnFlags as a bitmask of 8 different bit flags. There are some 
 * invariants that the server will hold:
 * 
 * - On Connection: When a user is connected will have their IS_ACTIVE
 *   bit set, to represent that the connection slot (on the server side)
 *   is in use. 
 * 
 * - On Login: After connecting, if a client logs in, then they will have 
 *   their IS_AUTH bit flag set as well. This implies the obvious: if you know 
 *   a user is logged in, that means they are also an active connection. 
 * 
 * - On Disconnect: When a connection disconnects, their connection slot becomes
 *   inactive and therefore unauthenticated. For our purposes, you can't have 
 *   an inactive connection that's authenticated.
 */
enum class ConnFlags : uint8_t {
    NONE       = 0,
    WANT_READ  = 1 << 0,
    WANT_WRITE = 1 << 1,
    WANT_CLOSE = 1 << 2,
    IS_ACTIVE  = 1 << 3, 
    IS_AUTH    = 1 << 4, 
};

/**
 * Performs a bitwise OR between two flags and returns the resulting flags.
 */
ConnFlags operator|(ConnFlags lhs, ConnFlags rhs);

/**
 * Performs a bitwise AND between two flags and returns the resulting flags.
 */
ConnFlags operator&(ConnFlags lhs, ConnFlags rhs);

/**
 * Performs a bitwise OR with an assignment and returns the resulting flags.
 */
ConnFlags operator|=(ConnFlags& lhs, ConnFlags rhs);

/**
 * Performs a bitwise negation on the connection flag and returns the resulting flags.
 */
ConnFlags operator~(ConnFlags flag);

/**
 * Performs bitwise AND and assignment and returns the resulting flags.
 */
ConnFlags operator&=(ConnFlags& lhs, ConnFlags rhs);

/**
 * Checks whether a flag is set in a given connection's mask/flags
 * @param mask The bitmask representing the flags/statuses for the connection.
 * @param flag A single flag that we're querying the bitmask for.
 * @return True if the flag is in the mask, otherwise false.
 */
bool has_flag(ConnFlags mask, ConnFlags flag);

/**
 * Prompts input for a value within a given range.
 * 
 * @param prompt Input prompt we're going to repeat.
 * @param min Minimum valid value.
 * @param max Maximum valid value.
 * @return The valid integer value that they inputted.
 */
int get_valid_input_range(std::string_view prompt, int min, int max);

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
void get_string_cin(std::string_view prompt, std::string& buffer);

#endif