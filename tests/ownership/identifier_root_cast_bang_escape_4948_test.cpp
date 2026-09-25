// §28.3 borrow-lifetime escapes spelled through the two SOURCE-PRESERVING
// operators, `x as T` and the postfix `x!` (issue #4948).
//
// Spec authority (v0.307):
//   * §17.4 *Type tests and ownership*: "For `as`, the result is additionally a
//     *borrow* of `x` (more precisely, a borrow with the same source as `x`),
//     regardless of whether `x` is owned or borrowed."
//   * §28.3 producer table: `x as T` is "Borrowed (source: same as `x`'s source
//     if `x` is borrowed; `x` itself if `x` is owned)".
//   * §17.8: "if `x` has ownership state *owned* …, `x!` is owned; if `x` is
//     borrowed, `x!` is borrowed with the same source."
//   * §28.3 borrow-lifetime rule, §28.10 (a `using var` "cannot be moved out
//     of, only borrowed from"), §28.11 borrowing-collection store rule.
//
// THE BUG. The escape checks root the escaping value with `identifierRoot`
// (or, for a holder local's provenance, `borrowSourceForExpr`), and neither
// looked through `as` / `!`. So a borrow of a scope-bound local escaped every
// check when it was spelled through either operator, although the bare
// `return x` / `x.Self()` / `var b := x` / `.Add(x)` twins are rejected.
// MEASURED on f321f94e (`bezelc --sanitize=address`): every REJECT row below
// compiles clean and runs into a heap-use-after-free in the caller's read of
// the returned or stored `Widget`.
//
// OBSERVABILITY. The pre-fix behaviour is a CLEAN compile followed by a UAF,
// so each REJECT row asserts the SPECIFIC diagnostic text AND `!r.ran`. Each
// ACCEPT control asserts an error-free compile, `ran`, a zero exit and the
// exact stdout.
//
// per §17.4, §17.8, §28.3, §28.10, §28.11, v0.307

#include <string>

#include "support/aot_support.hpp"  // runAot — the strict AOT oracle
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::runAot;

namespace {

bool hasErrorContaining(const JitResult& r, const std::string& needle) {
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.severity == bezel::Severity::Error &&
            d.message.find(needle) != std::string::npos)
            return true;
    return false;
}

int errorCount(const JitResult& r) {
    int n = 0;
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.severity == bezel::Severity::Error) ++n;
    return n;
}

void expectRejected(const std::string& program, const std::string& needle) {
    JitResult r = runAot(program);
    BEZEL_EXPECT_TRUE(hasErrorContaining(r, needle));
    BEZEL_EXPECT_FALSE(r.ran);
}

void expectAccepted(const std::string& program, const std::string& out) {
    JitResult r = runAot(program);
    BEZEL_EXPECT_EQ(errorCount(r), 0);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, out);
}

const char* kWidget =
    "module M; "
    "public class Widget "
    "  private _n: string; "
    "  public constructor(n: string) _n := n; end constructor; "
    "  public method Name(): string => _n; "
    "  public method Self(): Widget => this; "
    "end Widget; ";

// The issue's repro: an INSTANCE method (so a borrowed `Widget` return is
// well-formed) whose body is `BODY`; `Main` reads the returned borrow.
std::string retProgram(const std::string& body) {
    return std::string(kWidget) +
           "public class H "
           "  private owned _w: Widget; "
           "  public constructor() _w := new Widget(\"field\"); end constructor; "
           "  public method F(g: Widget): Widget "
           "    using var x := new Widget(\"x\"); " + body +
           "  end F; "
           "end H; "
           "public class Program "
           "  public static method Main(): int "
           "    using var g := new Widget(\"g\"); "
           "    using var h := new H(); "
           "    Console.WriteLine(h.F(g).Name()); "
           "    return 0; "
           "  end Main; "
           "end Program;";
}

// A store of `STORE` into the receiver's borrowing `Array<Widget>` field
// (§28.11), read back by `Main` after `F` returns.
std::string storeProgram(const std::string& stmt) {
    return std::string(kWidget) +
           "public class H "
           "  private owned _w: Widget; "
           "  private owned _items: Array<Widget>; "
           "  public constructor() "
           "    _w := new Widget(\"field\"); _items := new Array<Widget>(); "
           "  end constructor; "
           "  public method F() "
           "    using var x := new Widget(\"x\"); " + stmt +
           "  end F; "
           "  public method G(): string => _items[0].Name(); "
           "end H; "
           "public class Program "
           "  public static method Main(): int "
           "    using var h := new H(); "
           "    h.F(); "
           "    Console.WriteLine(h.G()); "
           "    return 0; "
           "  end Main; "
           "end Program;";
}

const char* kUsingVarReturnNeedle =
    "borrow returned from `x` would outlive its source — `using var` `x`";

const char* kHolderReturnNeedle = "returned `b` carries a borrow of `x`";

const char* kStoreNeedle =
    "borrow of `x` passed to `.Add(...)` on `_items` would outlive its source";

}  // namespace

// ===========================================================================
// REJECT — the direct return (`checkBorrowEscapesOnReturn`). The issue's
// ret2 / ret5 rows.
// ===========================================================================

BEZEL_TEST(identifier_root_cast_bang_escape_4948,
           return_cast_forgive_using_var_rejected_4948_28_3) {
    expectRejected(retProgram("return (x as Widget)!;"),
                   kUsingVarReturnNeedle);
}

BEZEL_TEST(identifier_root_cast_bang_escape_4948,
           return_cast_forgive_derived_call_rejected_4948_28_3) {
    expectRejected(retProgram("return (x as Widget)!.Self();"),
                   kUsingVarReturnNeedle);
}

// §17.4: the `as` result is a borrow of `x` "regardless of whether `x` is
// owned" — so a plain owned local (auto-dropped at scope end) is the same
// escape.
BEZEL_TEST(identifier_root_cast_bang_escape_4948,
           return_cast_forgive_plain_owned_local_rejected_4948_28_3) {
    expectRejected(
        retProgram("var y := new Widget(\"y\"); return (y as Widget)!;"),
        "borrow returned from `y` would outlive its source — owned local `y`");
}

// §17.8: an owned `x!` IS `x`, so `return x!;` of a `using var` is the same
// scope-bound move-out the bare `return x;` is rejected for (§28.3/§28.10).
BEZEL_TEST(identifier_root_cast_bang_escape_4948,
           return_forgive_using_var_rejected_4948_17_8_28_10) {
    expectRejected(retProgram("return x!;"),
                   "cannot return `using var` `x`");
}

// ===========================================================================
// REJECT — the one-hop holder (`borrowSourceForExpr` provenance, then
// `checkIndirectEscapeOnReturn`).
// ===========================================================================

BEZEL_TEST(identifier_root_cast_bang_escape_4948,
           holder_bound_from_cast_forgive_returned_rejected_4948_28_3) {
    expectRejected(retProgram("var b := (x as Widget)!; return b;"),
                   kHolderReturnNeedle);
}

BEZEL_TEST(identifier_root_cast_bang_escape_4948,
           holder_bound_from_forgive_returned_rejected_4948_17_8) {
    expectRejected(retProgram("var b := x!; return b;"), kHolderReturnNeedle);
}

BEZEL_TEST(identifier_root_cast_bang_escape_4948,
           holder_returned_through_cast_forgive_rejected_4948_28_3) {
    expectRejected(retProgram("var b := x; return (b as Widget)!;"),
                   kHolderReturnNeedle);
}

// ===========================================================================
// REJECT — the §28.11 borrowing-collection store (`.Add` argument source).
// ===========================================================================

BEZEL_TEST(identifier_root_cast_bang_escape_4948,
           collection_add_cast_forgive_rejected_4948_28_11) {
    expectRejected(storeProgram("_items.Add((x as Widget)!);"), kStoreNeedle);
}

BEZEL_TEST(identifier_root_cast_bang_escape_4948,
           collection_add_forgive_rejected_4948_28_11) {
    expectRejected(storeProgram("_items.Add(x!);"), kStoreNeedle);
}

// ===========================================================================
// ACCEPT controls — `as` / `!` over a value that outlives the return or the
// store, or a value-typed projection of the cast. Must stay accepted.
// ===========================================================================

// A receiver-owned field: a receiver-sourced borrow (§28.3 row 4).
BEZEL_TEST(identifier_root_cast_bang_escape_4948,
           return_cast_forgive_receiver_field_accepted_4948) {
    expectAccepted(retProgram("return (_w as Widget)!;"), "field\n");
}

BEZEL_TEST(identifier_root_cast_bang_escape_4948,
           return_cast_forgive_receiver_field_derived_accepted_4948) {
    expectAccepted(retProgram("return (_w as Widget)!.Self();"), "field\n");
}

// A value-typed projection of the cast is a copy, never a borrow.
BEZEL_TEST(identifier_root_cast_bang_escape_4948,
           value_projection_of_cast_accepted_4948) {
    expectAccepted(
        std::string(kWidget) +
            "public class Program "
            "  public static method Len(): int "
            "    using var x := new Widget(\"abcd\"); "
            "    return (x as Widget)!.Name().Length; "
            "  end Len; "
            "  public static method Main(): int "
            "    Console.WriteLine(Len()); "
            "    return 0; "
            "  end Main; "
            "end Program;",
        "4\n");
}

// The cast held by a local that does not outlive its source.
BEZEL_TEST(identifier_root_cast_bang_escape_4948,
           local_holder_of_cast_forgive_same_scope_accepted_4948) {
    expectAccepted(
        std::string(kWidget) +
            "public class Program "
            "  public static method Main(): int "
            "    using var x := new Widget(\"x\"); "
            "    var b := (x as Widget)!; "
            "    var c := x!; "
            "    Console.WriteLine(b.Name() + c.Self().Name()); "
            "    return 0; "
            "  end Main; "
            "end Program;",
        "xx\n");
}

// A same-scope borrowing collection: the receiver does not outlive `x`.
BEZEL_TEST(identifier_root_cast_bang_escape_4948,
           same_scope_collection_add_cast_forgive_accepted_4948) {
    expectAccepted(
        std::string(kWidget) +
            "public class Program "
            "  public static method Main(): int "
            "    using var x := new Widget(\"x\"); "
            "    var arr := new Array<Widget>(); "
            "    arr.Add((x as Widget)!); "
            "    arr.Add(x!); "
            "    Console.WriteLine(arr[0].Name() + arr[1].Name()); "
            "    return 0; "
            "  end Main; "
            "end Program;",
        "xx\n");
}

// The receiver-owned field stored into the receiver's own collection.
BEZEL_TEST(identifier_root_cast_bang_escape_4948,
           collection_add_cast_forgive_receiver_field_accepted_4948) {
    expectAccepted(storeProgram("_items.Add((_w as Widget)!);"), "field\n");
}
