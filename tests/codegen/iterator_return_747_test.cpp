// Spec-derived regression tests for fix #747 (R42 IR_FAIL) — a `return` inside a
// SYNC iterator method.
//
// Authority: §29.5 — "A method that uses `yield` cannot use plain `return`,
// including `return;`. Use `yield break` to exit early." This applies to both
// synchronous (IEnumerable<T>) and async (IAsyncEnumerable<T>) iterators.
//
// Bug: a sync iterator returns IEnumerable<T>, which is a *value* return
// context, so checkReturnForm only rejected a bare `return;` (via §21.7, with a
// misleading "must provide a value" message) and MISSED `return expr;` entirely
// — the value-carrying return reached codegen and emitted malformed IR
// ("Function return type does not match operand type of return inst!"), failing
// IR verification instead of producing a §29.5 diagnostic. The fix rejects ANY
// `return` in an iterator method up front. A `return` inside a lambda nested in
// an iterator is the lambda's own return and stays legal.

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <sys/wait.h>

#include "driver/Driver.hpp"
#include "support/scratch_dir.hpp"
#include "support/unique_stem.hpp"
#include "test_harness.hpp"

namespace {
namespace fs = std::filesystem;

// Per-process unique working dir, under the shared scratch base (never
// /tmp — see support/scratch_dir.hpp) and reaped at process exit.
static const std::filesystem::path& wtDir747iter() {
    return wtScratchDir("wt-747iter");
}

// True when `source` FAILS to compile (a diagnostic is raised — NOT a crash or
// malformed IR).
bool compileFails(const std::string& source) {
    auto dir = wtDir747iter();
    std::string stem = unique_stem("bezel_747iter_neg");
    std::string src = (dir / (stem + ".bzl")).string();
    std::string exe = (dir / stem).string();
    { std::ofstream out(src); out << source; }
    bezel::Driver driver;
    bezel::CompileResult r = driver.compileFile(src, exe);
    std::remove(src.c_str());
    std::remove(exe.c_str());
    return !r.success;
}

// Compile to a temp native exe and run it; exit code, or -1 on any failure.
int compileAndRun(const std::string& source) {
    auto dir = wtDir747iter();
    std::string stem = unique_stem("bezel_747iter");
    std::string src = (dir / (stem + ".bzl")).string();
    std::string exe = (dir / stem).string();
    { std::ofstream out(src); out << source; }
    bezel::Driver driver;
    bezel::CompileResult r = driver.compileFile(src, exe);
    int code = -1;
    if (r.success) {
        int status = std::system(exe.c_str());
        if (status != -1 && WIFEXITED(status)) code = WEXITSTATUS(status);
    }
    std::remove(src.c_str());
    std::remove(exe.c_str());
    return code;
}
}  // namespace

// §29.5: `return expr;` in a sync iterator — the headline. Must be a clean
// compile error (was: IR verification failure / compiler error).
BEZEL_TEST(iterator_return_747, sync_iterator_value_return_rejected) {
    BEZEL_EXPECT_TRUE(compileFails(R"(
module M;
static class F
  public static method Nums(): IEnumerable<int>
    yield return 1;
    return 2;
  end Nums;
  public static method Main(): int return 0; end Main;
end F;
)"));
}

// §29.5: bare `return;` in a sync iterator is equally illegal (previously a
// misleading §21.7 "must provide a value" message; still must fail).
BEZEL_TEST(iterator_return_747, sync_iterator_bare_return_rejected) {
    BEZEL_EXPECT_TRUE(compileFails(R"(
module M;
static class F
  public static method Nums(): IEnumerable<int>
    yield return 1;
    return;
  end Nums;
  public static method Main(): int return 0; end Main;
end F;
)"));
}

// §29.5: `yield break;` is the correct early exit — must still compile.
BEZEL_TEST(iterator_return_747, yield_break_still_compiles) {
    BEZEL_EXPECT_FALSE(compileFails(R"(
module M;
static class F
  public static method Nums(): IEnumerable<int>
    yield return 1;
    yield break;
  end Nums;
  public static method Main(): int return 0; end Main;
end F;
)"));
}

// §29.5: a `return` inside a lambda NESTED in an iterator is the LAMBDA's own
// return, not the iterator's — it must stay legal and behave correctly. The
// iterator yields f(4) = 5; Main sums the single element and returns it.
BEZEL_TEST(iterator_return_747, return_in_nested_lambda_still_legal) {
    BEZEL_EXPECT_EQ(compileAndRun(R"(
module M;
static class F
  public static method Nums(): IEnumerable<int>
    var f: (int) -> int := lambda(x) => do return x + 1; end;
    yield return f(4);
    yield break;
  end Nums;
  public static method Main(): int
    var sum := 0;
    for var v in F.Nums() do sum := sum + v; end;
    return sum;
  end Main;
end F;
)"), 5);
}
