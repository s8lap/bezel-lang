// Regression tests for issue #8918 — §28.3.4 / §28.3.8 (v0.359).
//
// Spec basis:
//
//   §28.3.4 Producers: "Parameter `p` (no modifier) | Borrowed (source:
//   caller)" — no row exempts a parameter of a built-in collection type
//   (`OwningArray<T>`, `Array<T>`, ... are reference types, K3.13).
//
//   §28.3.8 Borrow lifetime rules: "A borrowed local may not be captured by a
//   closure that may outlive the borrow's source." and, in the sink rule,
//   "While its source set is non-empty, a function value … may not be
//   returned from the enclosing method".
//
// A plain parameter of a USER class is rejected by issue #4709's leg
// (`isPlainBorrowParam`); that predicate ended in `isBorrowableType`, which
// classifies a declaration-less built-in collection class as NOT borrowable,
// so `return lambda() => w[0].v;` over `w: OwningArray<Cell>` -- and the alias
// `var b := w[0];` form (#8849's leg, rooted at the same predicate) -- compiled
// with no diagnostic and the caller's `f()` read freed memory (ASan
// heap-use-after-free). MEASURED on r30-c1 @ af51cca67.
//
// Value-typed parameters (`string`, `int`, a record) stay admitted: they are
// not borrows (§28.9 runtime-managed / value types).

#include <string>

#include "support/ownership_support.hpp"
#include "test_harness.hpp"

using bezel::test::hasErrorMessage;
using bezel::test::parseAndCheckOwnership;

namespace {

const char* kPrelude =
    "module P; "
    "import System; "
    "class Cell "
    "  public v: int; "
    "  public constructor(x: int) this.v := x; end constructor; "
    "end Cell; ";

const char* kMain =
    "public class Program "
    "  public static method Main(): int "
    "    return 0; "
    "  end Main; "
    "end Program;";

std::string directCapture(const std::string& param) {
    return "returned closure captures `" + param +
           "`, a plain (borrowing) parameter";
}

}  // namespace

// Issue #8918's own repro: the DIRECT capture of `w: OwningArray<Cell>`.
BEZEL_TEST(builtin_collection_param_closure_escape_8918,
           returned_closure_capturing_owning_array_param_rejected) {
    auto r = parseAndCheckOwnership(
        std::string(kPrelude) +
        "class H "
        "  public static method G(w: OwningArray<Cell>): () -> int "
        "    return lambda() => w[0].v; "
        "  end G; "
        "end H; " + kMain);
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasErrorMessage(r, directCapture("w")));
}

// The same parameter reached through an ALIAS of an element (#8849's leg).
BEZEL_TEST(builtin_collection_param_closure_escape_8918,
           returned_closure_capturing_element_alias_of_collection_param_rejected) {
    auto r = parseAndCheckOwnership(
        std::string(kPrelude) +
        "class H "
        "  public static method G(w: OwningArray<Cell>): () -> int "
        "    var b := w[0]; "
        "    return lambda() => b.v; "
        "  end G; "
        "end H; " + kMain);
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasErrorMessage(
        r, "returned closure captures `b`, an alias of the plain (borrowing) "
           "parameter `w`"));
}

// The plain (non-owning) collection class, a different head of the same set.
BEZEL_TEST(builtin_collection_param_closure_escape_8918,
           returned_closure_capturing_plain_array_param_rejected) {
    auto r = parseAndCheckOwnership(
        std::string(kPrelude) +
        "class H "
        "  public static method G(w: Array<Cell>): () -> int "
        "    return lambda() => w[0].v; "
        "  end G; "
        "end H; " + kMain);
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasErrorMessage(r, directCapture("w")));
}

// CONTROL (§28.9: `string` is runtime-managed, a copy carries no source): a
// plain `string` parameter captured by a returned closure stays admitted.
BEZEL_TEST(builtin_collection_param_closure_escape_8918,
           returned_closure_capturing_string_param_still_admitted) {
    auto r = parseAndCheckOwnership(
        std::string(kPrelude) +
        "class H "
        "  public static method G(s: string): () -> int "
        "    return lambda() => s.Length; "
        "  end G; "
        "end H; " + kMain);
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// CONTROL (§28.3.8: the closure does not outlive the call): a collection
// parameter captured by a closure that is only invoked inside the method.
BEZEL_TEST(builtin_collection_param_closure_escape_8918,
           collection_param_captured_by_locally_invoked_closure_still_admitted) {
    auto r = parseAndCheckOwnership(
        std::string(kPrelude) +
        "class H "
        "  public static method G(w: OwningArray<Cell>): int "
        "    var f := lambda() => w[0].v; "
        "    return f(); "
        "  end G; "
        "end H; " + kMain);
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}
