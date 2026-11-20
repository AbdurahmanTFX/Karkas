#include "kk/index/db.hpp"

#include <fmt/format.h>
#include <sqlite3.h>

#include <cstdlib>
#include <iostream>

namespace kk::index {

Stmt::Stmt(Stmt&& o) noexcept : db_(o.db_), s_(o.s_), err_(std::move(o.err_)) { o.s_ = nullptr; }
Stmt& Stmt::operator=(Stmt&& o) noexcept {
  if (this != &o) {
    if (s_ != nullptr) sqlite3_finalize(s_);
    db_ = o.db_;
    s_ = o.s_;
    err_ = std::move(o.err_);
    o.s_ = nullptr;
  }
  return *this;
}
Stmt::~Stmt() {
  if (s_ != nullptr) sqlite3_finalize(s_);
}

Stmt& Stmt::bind(int i, std::string_view v) {
  sqlite3_bind_text(s_, i, v.data(), static_cast<int>(v.size()), SQLITE_TRANSIENT);
  return *this;
}
Stmt& Stmt::bind(int i, std::int64_t v) {
  sqlite3_bind_int64(s_, i, v);
  return *this;
}
Stmt& Stmt::bind(int i, double v) {
  sqlite3_bind_double(s_, i, v);
  return *this;
}
Stmt& Stmt::bind_null(int i) {
  sqlite3_bind_null(s_, i);
  return *this;
}
Stmt& Stmt::bind_opt(int i, std::string_view v) { return v.empty() ? bind_null(i) : bind(i, v); }

bool Stmt::step() {
  int rc = sqlite3_step(s_);
  if (rc == SQLITE_ROW) return true;
  if (rc != SQLITE_DONE) err_ = sqlite3_errmsg(db_);
  return false;
}

kk::VoidResult Stmt::run() {
  while (step()) {
  }
  reset();
  if (!err_.empty()) {
    auto e = err_;
    err_.clear();
    return kk::fail("sqlite", e);
  }
  return {};
}

void Stmt::reset() {
  sqlite3_reset(s_);
  sqlite3_clear_bindings(s_);
}

std::string Stmt::text(int col) const {
  const auto* p = sqlite3_column_text(s_, col);
  if (p == nullptr) return {};
  return std::string(reinterpret_cast<const char*>(p), static_cast<std::size_t>(sqlite3_column_bytes(s_, col)));
}
std::int64_t Stmt::i64(int col) const { return sqlite3_column_int64(s_, col); }
double Stmt::f64(int col) const { return sqlite3_column_double(s_, col); }
bool Stmt::is_null(int col) const { return sqlite3_column_type(s_, col) == SQLITE_NULL; }

kk::Result<Db> Db::open(const std::filesystem::path& file, bool read_only) {
  Db db;
  int flags = read_only ? SQLITE_OPEN_READONLY : (SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE);
  flags |= SQLITE_OPEN_NOMUTEX;
  if (sqlite3_open_v2(file.string().c_str(), &db.db_, flags, nullptr) != SQLITE_OK) {
    std::string msg = db.db_ != nullptr ? sqlite3_errmsg(db.db_) : "out of memory";
    return kk::fail("sqlite", fmt::format("cannot open {}: {}", file.string(), msg));
  }
  sqlite3_busy_timeout(db.db_, 30000);  // another process may hold it for a whole update (ADR 0029)
  return db;
}

Db::Db(Db&& o) noexcept : db_(o.db_) { o.db_ = nullptr; }
Db& Db::operator=(Db&& o) noexcept {
  if (this != &o) {
    if (db_ != nullptr) sqlite3_close_v2(db_);
    db_ = o.db_;
    o.db_ = nullptr;
  }
  return *this;
}
Db::~Db() {
  if (db_ != nullptr) sqlite3_close_v2(db_);
}

kk::VoidResult Db::exec(std::string_view sql) {
  char* err = nullptr;
  std::string s(sql);
  if (sqlite3_exec(db_, s.c_str(), nullptr, nullptr, &err) != SQLITE_OK) {
    std::string msg = err != nullptr ? err : "unknown error";
    sqlite3_free(err);
    return kk::fail("sqlite", msg);
  }
  return {};
}

kk::Result<Stmt> Db::prepare(std::string_view sql) {
  sqlite3_stmt* s = nullptr;
  if (sqlite3_prepare_v2(db_, sql.data(), static_cast<int>(sql.size()), &s, nullptr) != SQLITE_OK) {
    return kk::fail("sqlite", fmt::format("{} in: {}", sqlite3_errmsg(db_), sql));
  }
  return Stmt(db_, s);
}

Stmt Db::q(std::string_view sql) {
  auto s = prepare(sql);
  if (!s) {
    std::cerr << "karkas: internal SQL error: " << s.error().message << "\n";
    std::abort();
  }
  return std::move(*s);
}

kk::VoidResult Db::begin() { return exec("BEGIN IMMEDIATE"); }
kk::VoidResult Db::commit() { return exec("COMMIT"); }
void Db::rollback() { (void)exec("ROLLBACK"); }

std::int64_t Db::data_version() {
  auto s = q("PRAGMA data_version");
  return s.step() ? s.i64(0) : 0;
}

std::string Db::last_error() const { return db_ != nullptr ? sqlite3_errmsg(db_) : ""; }

}  // namespace kk::index
