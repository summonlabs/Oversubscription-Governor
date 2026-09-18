// Oversubscription Governor test harness (dependency-free).
#ifndef OVERSUB_TEST_FRAMEWORK_HPP
#define OVERSUB_TEST_FRAMEWORK_HPP

#include <cstdio>
#include <functional>
#include <sstream>
#include <string>
#include <vector>

namespace otest {

struct TestCase {
  std::string name;
  std::function<void()> fn;
};

inline std::vector<TestCase>& registry() {
  static std::vector<TestCase> r;
  return r;
}

inline int& failures() {
  static int f = 0;
  return f;
}

inline const char*& current() {
  static const char* c = "";
  return c;
}

struct Registrar {
  Registrar(const char* name, std::function<void()> fn) { registry().push_back({name, std::move(fn)}); }
};

inline void fail(const std::string& msg) {
  std::fprintf(stderr, "  [FAIL] %s: %s\n", current(), msg.c_str());
  ++failures();
}

inline int run_all(int argc, char** argv) {
  std::setvbuf(stdout, nullptr, _IONBF, 0);
  std::string filter;
  if (argc > 1) filter = argv[1];
  int ran = 0;
  for (auto& t : registry()) {
    if (!filter.empty() && t.name.find(filter) == std::string::npos) continue;
    current() = t.name.c_str();
    std::printf("[ RUN ] %s\n", t.name.c_str());
    std::fflush(stdout);
    try {
      t.fn();
    } catch (const std::exception& e) {
      fail(std::string("uncaught exception: ") + e.what());
    } catch (...) {
      fail("uncaught unknown exception");
    }
    ++ran;
  }
  std::printf("%d tests ran, %d failures\n", ran, failures());
  return failures() == 0 ? 0 : 1;
}

}  // namespace otest

#define OVERSUB_TEST_IMPL \
  int main(int argc, char** argv) { return ::otest::run_all(argc, argv); }

#define TEST(name) \
  static void otest_fn_##name(); \
  static ::otest::Registrar otest_reg_##name(#name, otest_fn_##name); \
  static void otest_fn_##name()

#define CHECK(cond) \
  do { if (!(cond)) { ::otest::fail("CHECK failed: " #cond); } } while (0)

#define CHECK_EQ(a, b) \
  do { const auto& va_ = (a); const auto& vb_ = (b); \
       if (!(va_ == vb_)) { std::stringstream ss_; ss_ << "CHECK_EQ failed: " #a " == " #b " (" << va_ << " != " << vb_ << ")"; \
                            ::otest::fail(ss_.str()); } } while (0)

#define REQUIRE(cond) \
  do { if (!(cond)) { ::otest::fail("REQUIRE failed: " #cond); return; } } while (0)

#endif  // OVERSUB_TEST_FRAMEWORK_HPP
