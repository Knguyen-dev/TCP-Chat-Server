#include "db.hpp"

sqlite3 *db;

int init_db() {
  int rc = sqlite3_open("chat_app.db", &db);
  if (rc != SQLITE_OK) {
    fprintf(stderr, "Cannot open database: %s\n", sqlite3_errmsg(db));
    return -1;
  }

  char *sql = "CREATE TABLE IF NOT EXISTS users("
              "id INTEGER PRIMARY KEY AUTOINCREMENT, "
              "username TEXT UNIQUE, password TEXT);";
  
  rc = sqlite3_exec(db, sql, 0, 0, NULL);
  return (rc == SQLITE_OK) ? 0 : -1;
}

void close_db() {
  sqlite3_close_v2(db);
}

int insert_user(user_t& user) {
  sqlite3_stmt *res;
  char *sql = "INSERT INTO users (username, password) VALUES (?, ?);";

  if (sqlite3_prepare_v2(db, sql, -1, &res, 0) != SQLITE_OK) return -1;

  // Bind values to the '?' placeholders
  sqlite3_bind_text(res, 1, user.username, -1, SQLITE_STATIC);
  sqlite3_bind_text(res, 2, user.password, -1, SQLITE_STATIC);

  int step = sqlite3_step(res);
  if (step == SQLITE_DONE) {
    // Get the ID that was just generated automatically
    user.id = (uint32_t)sqlite3_last_insert_rowid(db);
    sqlite3_finalize(res);
    return 0;
  }

  sqlite3_finalize(res);
  return -1;
}

int get_user_by_username(std::string username, user_t& user) {
  sqlite3_stmt *res;
  char *sql = "SELECT id, username, password FROM users WHERE username = ?;";

  // Prepare the SQL statement
  if (sqlite3_prepare_v2(db, sql, -1, &res, 0) != SQLITE_OK) {
    return -1;
  }

  sqlite3_bind_text(res, 1, username.c_str(), -1, SQLITE_STATIC);
  int found = 0;
  if (sqlite3_step(res) == SQLITE_ROW) {
      user.id = sqlite3_column_int(res, 0);
      strcpy(user.username, (const char*)sqlite3_column_text(res, 1));
      strcpy(user.password, (const char*)sqlite3_column_text(res, 2));
      found = 1;
  }

  sqlite3_finalize(res);
  return found;
}
