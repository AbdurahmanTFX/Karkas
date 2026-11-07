#pragma once
// Minimal RAII wrapper over sqlite3 (prepared statements, transactions).
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

#include "kk/core/error.hpp"

struct sqlite3;
struct sqlite3_stmt;

namespace kk::index {

class Stmt {
 public:
  Stmt() = default;
  Stmt(sqlite3* db, sqlite3_stmt* s) : db_(db), s_(s) {}
  Stmt(Stmt&& o) noexcept;
  Stmt& operator=(Stmt&& o) noexcept;
  Stmt(const Stmt&) = delete;
  Stmt& operator=(const Stmt&) = delete;
  ~Stmt();

  Stmt& bind(int i, std::string_view v);
  Stmt& bind(int i, const char* v) { return bind(i, std::string_view(v)); }
  Stmt& bind(int i, const std::string& v) { return bind(i, std::string_view(v)); }
  Stmt& bind(int i, std::int64_t v);
  Stmt& bind(int i, int v) { return bind(i, static_cast<std::int64_t>(v)); }
  Stmt& bind(int i, double v);
  Stmt& bind_null(int i);
  Stmt& bind_opt(int i, std::string_view v);  // empty → NULL

  // Returns true while a row is available; false when done. Errors → false + error().
  bool step();
  kk::VoidResult run();  // step to completion, then reset
  void reset();

  [[nodiscard]] std::string text(int col) const;
  [[nodiscard]] std::int64_t i64(int col) const;
  [[nodiscard]] double f64(int col) const;
  [[nodiscard]] bool is_null(int col) const;
  [[nodiscard]] const std::string& error() const { return err_; }

 private:
  sqlite3* db_ = nullptr;
  sqlite3_stmt* s_ = nullptr;
  std::string err_;
};

class Db {
 public:
  static kk::Result<Db> open(const std::filesystem::path& file, bool read_only = false);
  Db() = default;
  Db(Db&& o) noexcept;
  Db& operator=(Db&& o) noexcept;
  Db(const Db&) = delete;
  Db& operator=(const Db&) = delete;
  ~Db();

  kk::VoidResult exec(std::string_view sql);
  kk::Result<Stmt> prepare(std::string_view sql);
  // Prepares or fails hard (programming errors in fixed SQL).
  Stmt q(std::string_view sql);
  kk::VoidResult begin();
  kk::VoidResult commit();
  void rollback();
  [[nodiscard]] std::int64_t data_version();  // PRAGMA data_version (6.4)
  [[nodiscard]] std::string last_error() const;
  [[nodiscard]] sqlite3* raw() const { return db_; }

 private:
  sqlite3* db_ = nullptr;
};

}  // namespace kk::index
