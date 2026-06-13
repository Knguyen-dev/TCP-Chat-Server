#include "db.hpp"

sqlite3 *db;

// Gives external linkage to our constant globals.
// NOTE: Then in the header file, we declare these variables as `extern const
// char* const` which creates a forward declaration, saying they're defined
// elsewhere and are constant.
extern const char *const DB_PATH = "app.db";
extern const char *const DB_TEST_PATH = "test.db";

int init_db(const char *db_path) {
  int rc = sqlite3_open(db_path, &db);
  if (rc != SQLITE_OK) {
    fprintf(stderr, "Cannot open database: %s\n", sqlite3_errmsg(db));
    return -1;
  }

  const char *sql = "CREATE TABLE IF NOT EXISTS users("
                    "id INTEGER PRIMARY KEY AUTOINCREMENT, "
                    "username TEXT UNIQUE, password TEXT);";

  rc = sqlite3_exec(db, sql, 0, 0, NULL);
  return (rc == SQLITE_OK) ? 0 : -1;
}

void close_db() {
  sqlite3_close_v2(db);

  // TODO: Kind of hacky to delete the test database file like this, but it
  // works for now.
  if (remove(DB_TEST_PATH) != 0) {
    fprintf(stderr, "Error deleting test database file: %s\n", strerror(errno));
  }
}

int insert_user(user_t &user) {
  sqlite3_stmt *res;
  const char *sql = "INSERT INTO users (username, password) VALUES (?, ?);";

  if (sqlite3_prepare_v2(db, sql, -1, &res, 0) != SQLITE_OK)
    return -1;

  // Bind values to the '?' placeholders
  sqlite3_bind_text(res, 1, user.username.data(), -1, SQLITE_STATIC);
  sqlite3_bind_text(res, 2, user.password.data(), -1, SQLITE_STATIC);

  int step = sqlite3_step(res);
  if (step == SQLITE_DONE) {
    // Get the ID that was just generated automatically
    user.user_id = (uint32_t)sqlite3_last_insert_rowid(db);
    sqlite3_finalize(res);
    return 0;
  }

  sqlite3_finalize(res);
  return -1;
}

int get_user_by_username(const std::string &username, user_t &user) {
  sqlite3_stmt *res;
  const char *sql =
      "SELECT id, username, password FROM users WHERE LOWER(username) = ?;";

  // Prepare the SQL statement
  if (sqlite3_prepare_v2(db, sql, -1, &res, 0) != SQLITE_OK) {
    return -1;
  }

  sqlite3_bind_text(res, 1, username.c_str(), -1, SQLITE_STATIC);
  int found = 0;
  if (sqlite3_step(res) == SQLITE_ROW) {
    user.user_id = sqlite3_column_int(res, 0);
    const char *sql_username = (const char *)sqlite3_column_text(res, 1);
    const char *sql_password = (const char *)sqlite3_column_text(res, 2);
    user.username =
        sql_username ? sql_username : ""; // NOTE: sqlite3 can return NULL
    user.password = sql_password ? sql_password : "";
    found = 1;
  }

  sqlite3_finalize(res);
  return found;
}
