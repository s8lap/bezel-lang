// Regression tests for #8981: a READ of an EXPRESSION-bodied function-typed
// property leaks the closure its getter created — once per read.
//
// Spec (v0.359):
//  - §23.2: the expression-bodied property `property P: T => expr;` — the
//    body IS the getter's value;
//  - §27.5: "Captured variables are heap-allocated in a closure object";
//  - §28.9: "Runtime management governs the *function value itself* — the
//    heap-allocated closure object that backs it", whose "lifetime is managed
//    by the runtime without user involvement";
//  - §28.3.11: the discharge happens "at the end of the full expression in
//    which it is produced" — here, at the end of the read.
//
// THE DEFECT. #8763 gave the BLOCK-bodied function-typed getter a reference
// protocol: the getter is emitted OUT OF LINE, it mints ONE reference for the
// reader, and the reader's own discharge (a binding local's scope end, a
// direct call, a discarded read) gives it back. The EXPRESSION-bodied getter
// (§23.2) has no frame of its own — its body is INLINED in the READER's
// frame — so a LAMBDA-LITERAL body allocates its closure cell THERE, in the
// reader. The reader therefore holds a +1 that no discharge site gave back.
//
// MEASURED on the base (b916d130b) with `bezelc --sanitize=address`, 200
// reads of `property EP: (int) -> int => lambda(x: int) => x + 2;`:
//   "SUMMARY: AddressSanitizer: 14400 byte(s) leaked in 400 allocation(s)"
// — 72 B per read (the 64 B closure cell + its 8 B environment), one per
// read, unbounded. The issue's own literal repro (ONE read) prints
// "SUMMARY: AddressSanitizer: 72 byte(s) leaked in 2 allocation(s)".
// Every BLOCK-bodied twin of the probes below is clean on the same base, so
// the defect is the EXPRESSION-bodied SPELLING, not the property read.
//
// Rows run under runAotChecked (generated-code ASan + leak check); every read
// is in a loop, so a leak cannot hide in sanitizer noise.
//
// Rows 1-5 are the defect's own shapes, all RED on the base. Row 6 is the
// BOUNDARY control: the BLOCK-bodied getter's twin, which #8763's protocol
// already discharges — the fix must not disturb it (it would over-release).
//
// per §23.2, §27.5, §28.3.11, §28.9 (v0.359)

#include <string>

#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAotChecked;

namespace {
std::string stdoutChecked(const std::string& program) {
    auto r = runAotChecked(program);
    return (r.ran && r.exitCode == 0) ? r.stdoutText
                                      : std::string("<did-not-run>");
}
}  // namespace

// Row 1 — the `.` spelling of the READ, bound to a local. The binding local
// owns the reference the inlined getter minted, and gives it back at its
// scope end.
BEZEL_TEST(property_expr_getter_closure_release_8981,
           expr_getter_read_bound_released_200x_spec_28_9) {
    BEZEL_EXPECT_EQ(stdoutChecked(
        "module M; "
        "public class H "
        "  public property EP: (int) -> int => lambda(x: int) => x + 2; "
        "end H; "
        "public static class Program "
        "  public static method Main(args: Array<string>): int "
        "    using var h := new H(); "
        "    var i := 0; "
        "    var tot := 0; "
        "    while i < 200 do "
        "      var g := h.EP; "
        "      tot := tot + g(1); "
        "      i := i + 1; "
        "    end; "
        "    Console.Write(tot); "
        "    return 0; "
        "  end; "
        "end Program;"),
        "600");
}

// Row 2 — the `?.` spelling of the READ, bound to a local (§7.5 lifts the
// result to a nullable function value; the value is a plain pointer and the
// discharge is null-safe).
BEZEL_TEST(property_expr_getter_closure_release_8981,
           expr_getter_safe_nav_read_bound_released_200x_spec_7_5) {
    BEZEL_EXPECT_EQ(stdoutChecked(
        "module M; "
        "public class H "
        "  public property EP: (int) -> int => lambda(x: int) => x + 2; "
        "end H; "
        "public static class Program "
        "  public static method Main(args: Array<string>): int "
        "    using var h := new H(); "
        "    var hn: H? := h; "
        "    var i := 0; "
        "    var tot := 0; "
        "    while i < 200 do "
        "      var g := hn?.EP; "
        "      if g <> null then tot := tot + g!(1); end; "
        "      i := i + 1; "
        "    end; "
        "    Console.Write(tot); "
        "    return 0; "
        "  end; "
        "end Program;"),
        "600");
}

// Row 3 — the `.` spelling of the CALL. The call takes no ownership of the
// closure, so the reference the inlined getter minted is given back at the
// end of the call expression (§28.3.11).
BEZEL_TEST(property_expr_getter_closure_release_8981,
           expr_getter_direct_call_released_200x_spec_28_3_11) {
    BEZEL_EXPECT_EQ(stdoutChecked(
        "module M; "
        "public class H "
        "  public property EP: (int) -> int => lambda(x: int) => x + 2; "
        "end H; "
        "public static class Program "
        "  public static method Main(args: Array<string>): int "
        "    using var h := new H(); "
        "    var i := 0; "
        "    var tot := 0; "
        "    while i < 200 do "
        "      tot := tot + h.EP(1); "
        "      i := i + 1; "
        "    end; "
        "    Console.Write(tot); "
        "    return 0; "
        "  end; "
        "end Program;"),
        "600");
}

// Row 4 — the `?.` spelling of the CALL (§7.5: the member is resolved as
// `x.M` is, so the property's function value is invoked).
BEZEL_TEST(property_expr_getter_closure_release_8981,
           expr_getter_safe_nav_call_released_200x_spec_7_5) {
    BEZEL_EXPECT_EQ(stdoutChecked(
        "module M; "
        "public class H "
        "  public property EP: (int) -> int => lambda(x: int) => x + 2; "
        "end H; "
        "public static class Program "
        "  public static method Main(args: Array<string>): int "
        "    using var h := new H(); "
        "    var hn: H? := h; "
        "    var i := 0; "
        "    var tot := 0; "
        "    while i < 200 do "
        "      tot := tot + (hn?.EP(1) ?? 0); "
        "      i := i + 1; "
        "    end; "
        "    Console.Write(tot); "
        "    return 0; "
        "  end; "
        "end Program;"),
        "600");
}

// Row 5 — the DISCARDED read (§21.9: "the result is silently discarded").
// The read still produced a reference, so the statement must give it back.
BEZEL_TEST(property_expr_getter_closure_release_8981,
           expr_getter_discarded_read_released_200x_spec_21_9) {
    BEZEL_EXPECT_EQ(stdoutChecked(
        "module M; "
        "public class H "
        "  public property EP: (int) -> int => lambda(x: int) => x + 2; "
        "end H; "
        "public static class Program "
        "  public static method Main(args: Array<string>): int "
        "    using var h := new H(); "
        "    var i := 0; "
        "    while i < 200 do "
        "      h.EP; "
        "      i := i + 1; "
        "    end; "
        "    Console.Write(\"done\"); "
        "    return 0; "
        "  end; "
        "end Program;"),
        "done");
}

// Row 6 — the BOUNDARY control (green on the base): the BLOCK-bodied
// getter's twin. #8763's protocol already discharges that one (the getter
// is emitted out of line and mints the reader's reference), so the fix must
// leave it exactly as it is — a fix that admitted the BLOCK form's `?.`
// spelling would release a reference nobody minted.
BEZEL_TEST(property_expr_getter_closure_release_8981,
           block_getter_twin_unchanged_clean_spec_28_9) {
    BEZEL_EXPECT_EQ(stdoutChecked(
        "module M; "
        "public class H "
        "  private _c: (int) -> int; "
        "  public constructor() _c := lambda(x: int) => x + 2; end; "
        "  public property BP: (int) -> int "
        "    get => do "
        "      return _c; "
        "    end; "
        "  end; "
        "end H; "
        "public static class Program "
        "  public static method Main(args: Array<string>): int "
        "    using var h := new H(); "
        "    var hn: H? := h; "
        "    var i := 0; "
        "    var tot := 0; "
        "    while i < 200 do "
        "      var g := h.BP; "
        "      tot := tot + g(1); "
        "      var gb := hn?.BP; "
        "      if gb <> null then tot := tot + gb!(1); end; "
        "      tot := tot + h.BP(1); "
        "      tot := tot + (hn?.BP(1) ?? 0); "
        "      i := i + 1; "
        "    end; "
        "    Console.Write(tot); "
        "    return 0; "
        "  end; "
        "end Program;"),
        "2400");
}
