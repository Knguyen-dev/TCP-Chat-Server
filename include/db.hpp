#ifndef DB_H
#define DB_H

#include "shared.hpp"

// C-library 
extern "C" {
  #include <sqlite3.h>
}

// NOTE: Forward declarations for our constant globals
extern const char* const DB_PATH;
extern const char* const DB_TEST_PATH;

/**
 * Initializes sqlite database schema and connection.
 * @param db_path File path for the sqlite database file.
 * @return 0 on success, otherwise -1
 */
int init_db(const char* db_path);

/**
 * Closes the database connection with Sqlite
 */
void close_db();

/**
 * Attempts to insert a new user into the database.
 * @param user User to be inserted into the database.
 * @return 0 on success, otherwise -1
 */
int insert_user(user_t& user);

/**
 * Attempts to query user by their username
 * 
 * @param username Username of the user being searched.
 * @param user Empty struct that'll be populated with user information if a user is found.
 * @return 1 if user was found, 0 if they didn't exist, and -1 if we had an error.
 */
int get_user_by_username(const std::string& username, user_t& user);

#endif