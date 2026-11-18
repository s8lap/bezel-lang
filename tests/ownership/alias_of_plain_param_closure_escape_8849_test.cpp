// Regression tests for issue #8849 — §28.3 *Provenance* clause 1 (v0.359).
//
// Spec basis: §28.3.8 "Borrow lifetime rules" (v0.359).
//
//   "A borrowed local may not be captured by a closure that may outlive the
//   borrow's source."
//
//   *Provenance*, clause 1: "A **lambda literal** contributes the sources of
//   the borrows it captures — a captured borrowed local or parameter that
//   binding's source; …"
//
//   *Announcement-erasing positions (the sink rule)*: "While its source set is
//   non-empty, a function value … may not be returned from the enclosing
//   method."
//
// A PLAIN (non-`owned`/`ref`/`out`/`scoped`) class/interface-typed PARAMETER's
// own source is "this call" — the premise issue #4709's leg already reads
// (`isPlainBorrowParam`), and a returned closure necessarily outlives the
// call, so a DIRECT capture of one is rejected.
//
// An ALIAS carries the source set of its initializer (§28.3's producers:
// "Local-variable read `x` where `x` is borrowed | Borrowed (source: same as
// `x`'s source)" — and "Parameter `p` (no modifier) | Borrowed (source:
// caller)"): `var b := w;` binds `b` to a BORROW of the plain parameter `w`,
// so a captured `b` contributes `w`'s source, and the closure's source set
// is non-empty exactly as the direct capture's is.
//
// MEASURED (issue #8849's repro, on r30-c1 @ 02d621783): the program below
// compiled with ZERO diagnostics — the alias's provenance was dropped on the
// way in, so the escape check never saw it — and the caller's `f()` then read
// freed memory (`ERROR: AddressSanitizer: heap-use-after-free`). The DIRECT
// capture of the same parameter was rejected.

#include <string>

#include "support/ownership_support.hpp"
#include "test_harness.hpp"

using bezel::test::hasErrorMessage;
using bezel::test::parseAndCheckOwnership;

namespace {

// The rule-specific fragment of the §28.3 alias diagnostic: the CAPTURED
// binding and the PARAMETER it is an alias of. Stable across the wording of
// the rest of the sentence (the support header's rule on stable needles).
std::string aliasOfParam(const std::string& captured,
                         const std::string& param) {
    return "returned closure captures `" + captured +
           "`, an alias of the plain (borrowing) parameter `" + param + "`";
}

}  // namespace

// =============================================================================
// NEGATIVE — a closure capturing an ALIAS of a plain borrow parameter must be
// rejected when it escapes via return (§28.3 *Provenance* clause 1, v0.359).
// =============================================================================

// Issue #8849's own repro, verbatim: `b` is an ALIAS (`var b := w;`) of the
// plain parameter `w`, so the captured `b` contributes `w`'s source — "this
// call" — and the returned closure necessarily outlives it.
BEZEL_TEST(alias_of_plain_param_closure_escape_8849,
           returned_closure_capturing_alias_of_plain_param_rejected) {
    auto r = parseAndCheckOwnership(
        "module P; "
        "import System; "
        "class Cell "
        "  public v: int; "
        "  public constructor(x: int) this.v := x; end constructor; "
        "end Cell; "
        "class H "
        "  public static method G(w: Cell): () -> int "
        "    var b := w; "
        "    return lambda() => b.v; "
        "  end G; "
        "end H; "
        "public class Program "
        "  public static method Main(): int "
        "    var f: () -> int := lambda() => 0; "
        "    do "
        "      var c := new Cell(8); "
        "      f := H.G(c); "
        "    end; "
        "    Console.WriteLine(f()); "
        "    return 0; "
        "  end Main; "
        "end Program;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(
        hasErrorMessage(r, aliasOfParam("b", "w")));
}

// The MULTI-HOP alias: `var c := b;` binds `c` to a borrow of `b`, which is a
// borrow of `w` — the chain is followed to the plain parameter at its root, so
// the same rejection fires and names the CAPTURED binding and the parameter.
BEZEL_TEST(alias_of_plain_param_closure_escape_8849,
           returned_closure_capturing_multihop_alias_of_plain_param_rejected) {
    auto r = parseAndCheckOwnership(
        "module P; "
        "import System; "
        "class Cell "
        "  public v: int; "
        "  public constructor(x: int) this.v := x; end constructor; "
        "end Cell; "
        "class H "
        "  public static method G(w: Cell): () -> int "
        "    var b := w; "
        "    var c := b; "
        "    return lambda() => c.v; "
        "  end G; "
        "end H; "
        "public class Program "
        "  public static method Main(): int "
        "    return 0; "
        "  end Main; "
        "end Program;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(
        hasErrorMessage(r, aliasOfParam("c", "w")));
}

// The `const` SPELLING of the same alias: §21.1's `const` is a separate AST
// node (`LocalConstDecl`) from `var`'s (`LocalDecl`), and it binds the same
// borrow — so the same rejection fires.
BEZEL_TEST(alias_of_plain_param_closure_escape_8849,
           returned_closure_capturing_const_alias_of_plain_param_rejected) {
    auto r = parseAndCheckOwnership(
        "module P; "
        "import System; "
        "class Cell "
        "  public v: int; "
        "  public constructor(x: int) this.v := x; end constructor; "
        "end Cell; "
        "class H "
        "  public static method G(w: Cell): () -> int "
        "    const b := w; "
        "    return lambda() => b.v; "
        "  end G; "
        "end H; "
        "public class Program "
        "  public static method Main(): int "
        "    return 0; "
        "  end Main; "
        "end Program;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(
        hasErrorMessage(r, aliasOfParam("b", "w")));
}

// A MEMBER read is a borrow of the storage's HOLDER (§28.3 K1.5: "Reads of
// storage lend: a field, property, indexer, or slot read is a borrow whose
// source is the storage's holder"), so `var b := w.inner;` is a borrow of `w`
// too, and the captured `b` contributes `w`'s source — the same rejection.
BEZEL_TEST(alias_of_plain_param_closure_escape_8849,
           returned_closure_capturing_member_read_alias_rejected) {
    auto r = parseAndCheckOwnership(
        "module P; "
        "import System; "
        "class Inner "
        "  public v: int; "
        "  public constructor(x: int) this.v := x; end constructor; "
        "end Inner; "
        "class Widget "
        "  public inner: Inner; "
        "  public constructor(i: Inner) this.inner := i; end constructor; "
        "end Widget; "
        "class H "
        "  public static method G(w: Widget): () -> int "
        "    var b := w.inner; "
        "    return lambda() => b.v; "
        "  end G; "
        "end H; "
        "public class Program "
        "  public static method Main(): int "
        "    return 0; "
        "  end Main; "
        "end Program;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(
        hasErrorMessage(r, aliasOfParam("b", "w")));
}
