// Regression tests for #8763: a lambda-valued local in a FUNCTION-TYPED block
// getter leaks its closure, once per read.
//
// Spec (v0.359):
//  - §27.5: "Captured variables are heap-allocated in a closure object";
//  - §28.9: "Runtime management governs the *function value itself* — the
//    heap-allocated closure object that backs it", whose "lifetime is managed
//    by the runtime without user involvement", and "a plain `var` of function
//    type scopes its captures identically" (to its enclosing scope);
//  - §28.9: "A getter body whose result is a fresh runtime-managed production
//    … remains admissible — runtime-managed values are exempt from the
//    ceremony — and the implementation reclaims the production; **how** is
//    not specified".
// The closure local formed in the getter body and handed to the reader holds
// the only reference to its closure object; the implementation reclaims it,
// and the reference the reader holds is discharged at the reader's scope end.
//
// The getter body did not run the per-body closure-local analysis and did not
// publish its declared return type, so the getter neither released its own
// closure locals nor minted the reader's reference: MEASURED on r30-c1
// (1128406b7) with `bezelc --sanitize=address` on the issue's literal repro —
// "SUMMARY: AddressSanitizer: 3600 byte(s) leaked in 100 allocation(s)" over
// 50 reads.
//
// Rows run under runAotChecked (checked disposal + generated-code ASan +
// leak check); the getter is read in a loop so the leak cannot hide in
// sanitizer noise.
//
// Rows 4 and 5 are the FIX-FORWARD's: #8763 taught the reader side of
// the block getter's protocol only to the read sites that TAKE ownership (a
// binding local, a store, an argument, a return). A read that is INVOKED
// directly (`h.P()`) takes no ownership — the call consumes the closure and
// drops it — so the reference the getter handed over had no owner and leaked.
// They are RED on #8763's commit (621997773) and green with the fix; the
// three rows above them are red on the base the fix was written against.
//
// Rows 6 and 7 are the FIX-FORWARD 2's (the pair 621997773 + cf92fa30a
// was REJECTED for two NEW leaks on shapes the r30-c1 tip is clean on):
//   * a NULLABLE function-typed getter (`(() -> int)?`) — the getter minted
//     the reader's reference for EVERY function-kind type, while the reader
//     predicate excludes a nullable one, so nothing ever discharged it;
//   * a `?.` read of a getter — §7.5 lifts the result to an always-nullable
//     type, and a nullable function value has no owner for a closure
//     reference, so that spelling reads a BORROW (§28.9: "a getter never
//     transfers a reclamation obligation to its reader") and its getter must
//     not mint.
// Both rows are RED on the pair and on the r30-c1 tip (a function-typed
// getter's body was not analysed there, so the getter's own closure local
// leaked as well) and green with this fix.
//
// per §27.5, §28.9, §23.3, §23.4 (v0.359)

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

// The issue's literal repro: a function-typed property whose BLOCK-form getter
// forms a lambda local (capturing `this`) and returns it.
BEZEL_TEST(property_block_getter_closure_release_8763,
           block_getter_closure_local_released_50x_spec_28_9) {
    BEZEL_EXPECT_EQ(stdoutChecked(
        "module M; "
        "public class H "
        "  public n: int; "
        "  public constructor(x: int) n := x; end; "
        "  public property Mk: () -> int "
        "    get => do "
        "      var g := lambda(): int => n + 1; "
        "      return g; "
        "    end; "
        "  end; "
        "end H; "
        "public static class Program "
        "  public static method Main(args: Array<string>): int "
        "    var i := 0; "
        "    var tot := 0; "
        "    using var h := new H(3); "
        "    while i < 50 do var f := h.Mk; tot := tot + f(); i := i + 1; end; "
        "    Console.Write(tot); "
        "    return 0; "
        "  end; "
        "end Program;"),
        "200");
}

// The capture-free twin of the same shape — the closure's environment is
// empty, so the leak is the closure cell alone.
BEZEL_TEST(property_block_getter_closure_release_8763,
           block_getter_capture_free_closure_released_50x_spec_28_9) {
    BEZEL_EXPECT_EQ(stdoutChecked(
        "module M; "
        "public class H "
        "  public property Mk: () -> int "
        "    get => do "
        "      var g := lambda(): int => 42; "
        "      return g; "
        "    end; "
        "  end; "
        "end H; "
        "public static class Program "
        "  public static method Main(args: Array<string>): int "
        "    var i := 0; "
        "    var tot := 0; "
        "    using var h := new H(); "
        "    while i < 50 do var f := h.Mk; tot := tot + f(); i := i + 1; end; "
        "    Console.Write(tot); "
        "    return 0; "
        "  end; "
        "end Program;"),
        "2100");
}

// The getter body also holds a closure local that is NOT returned: the
// scope-end release of THAT local is the #6766 mechanism extended to a
// function-typed getter (the body is analysed like any other body).
BEZEL_TEST(property_block_getter_closure_release_8763,
           block_getter_unreturned_closure_local_released_50x_spec_28_9) {
    BEZEL_EXPECT_EQ(stdoutChecked(
        "module M; "
        "public class H "
        "  public n: int; "
        "  public constructor(x: int) n := x; end; "
        "  public property Mk: () -> int "
        "    get => do "
        "      var spare := lambda(): int => 7; "
        "      var g := lambda(): int => n + 1; "
        "      return g; "
        "    end; "
        "  end; "
        "end H; "
        "public static class Program "
        "  public static method Main(args: Array<string>): int "
        "    var i := 0; "
        "    var tot := 0; "
        "    using var h := new H(3); "
        "    while i < 50 do var f := h.Mk; tot := tot + f(); i := i + 1; end; "
        "    Console.Write(tot); "
        "    return 0; "
        "  end; "
        "end Program;"),
        "200");
}

// fix #8763 (follow-up) — the DIRECT CALL `h.P()`.
//
// #8763 gave a FUNCTION-TYPED block getter a protocol: the getter hands the
// reader ONE reference (it retains one for a BORROWED value — a field's
// closure — or transfers the returned local's own +1) and the reader gives
// it back. The reader-side half was taught to the sites that TAKE ownership
// (a binding local, a store, an argument, a return). A read that is INVOKED
// directly takes no ownership: the call consumes the closure and drops it,
// and no binding site ever owns the reference the getter handed over.
//
// MEASURED on the #8763 commit (621997773), 200 iterations of this program:
//   "SUMMARY: AddressSanitizer: 72 byte(s) leaked in 2 allocation(s)"
// — the field's closure, over-retained by the getter and never given back.
//
// The row loops 200 times so the leak cannot hide in the sanitizer's noise,
// and it reads the DATA (the closure's own result) so a fix that misreads
// the value cannot pass it.
BEZEL_TEST(property_block_getter_closure_release_8763,
           block_getter_direct_call_reference_released_200x_spec_28_9) {
    BEZEL_EXPECT_EQ(stdoutChecked(
        "module M; "
        "public class H "
        "  private f: () -> int; "
        "  public constructor() f := lambda(): int => 5; end; "
        "  public property P: () -> int "
        "    get => do return f; end; "
        "  end; "
        "end H; "
        "public static class Program "
        "  public static method Main(args: Array<string>): int "
        "    var i := 0; "
        "    var tot := 0; "
        "    using var h := new H(); "
        "    while i < 200 do tot := tot + h.P(); i := i + 1; end; "
        "    Console.Write(tot); "
        "    return 0; "
        "  end; "
        "end Program;"),
        "1000");
}

// fix #8763 (follow-up) — the DIRECT CALL with the field REPLACED every
// iteration.
//
// The unbounded twin of the row above: `Reset()` replaces the field's
// closure each iteration, so the reference the getter handed over is a
// reference to a DIFFERENT closure every time, and the leak grows without
// bound instead of saturating at one closure.
//
// MEASURED on the #8763 commit (621997773), 200 iterations of this program:
//   "SUMMARY: AddressSanitizer: 14472 byte(s) leaked in 402 allocation(s)"
// — one closure (72 B, in 2 allocations) per iteration.
//
// The printed total (3599) is the program's own: 5 from the constructor's
// closure, then 6 + 6 + 6 per iteration after the first reset.
BEZEL_TEST(property_block_getter_closure_release_8763,
           block_getter_direct_call_after_field_reset_released_200x_spec_28_9) {
    BEZEL_EXPECT_EQ(stdoutChecked(
        "module M; "
        "public class H "
        "  public n: int; "
        "  private f: () -> int; "
        "  public constructor(x: int) n := x; f := lambda(): int => 5; end; "
        "  public method Reset() f := lambda(): int => 6; end; "
        "  public property P: () -> int "
        "    get => do return f; end; "
        "  end; "
        "  public property T: () -> int "
        "    get => do return this.f; end; "
        "  end; "
        "  public property W: () -> int "
        "    get => do try return f; finally n := n + 1; end; end; "
        "  end; "
        "end H; "
        "public static class Program "
        "  public static method Main(args: Array<string>): int "
        "    var i := 0; "
        "    var tot := 0; "
        "    using var h := new H(3); "
        "    while i < 200 do "
        "      tot := tot + h.P(); "
        "      h.Reset(); "
        "      tot := tot + h.T() + h.W(); "
        "      i := i + 1; "
        "    end; "
        "    Console.Write(tot); "
        "    return 0; "
        "  end; "
        "end Program;"),
        "3599");
}

// fix #8763 (fix-forward 2) — the NULLABLE function-typed block getter.
//
// #8763's getter side mints the reader's +1 closure reference for EVERY
// getter whose declared type is a function type, while the reader side
// (`blockGetterMintsClosureRef`) admits a NON-NULLABLE one only. A nullable
// property (`(() -> int)?`) therefore got a reference no read site ever
// discharged: one closure per read, unbounded.
//
// MEASURED on the #8763 pair (621997773 + cf92fa30a) with `bezelc
// --sanitize=address` on this program (200 reads):
//   "SUMMARY: AddressSanitizer: 14328 byte(s) leaked in 398 allocation(s)"
// — one closure per read, on a shape the r30-c1 tip is CLEAN on.
//
// §28.9 (v0.359): "a getter never transfers a reclamation obligation to its
// reader" — so a getter whose reader cannot discharge a reference must not
// mint one. The getter's mint is now gated on the SAME shared predicate the
// reader side asks, so the two halves cannot disagree.
//
// The row's getter body ALSO holds a closure local it does not return, so
// the row is red on the r30-c1 tip for that reason too (a function-typed
// getter's body was not analysed there — #6766's per-body analysis skipped
// it) and green only when BOTH halves hold: the body is analysed AND the
// getter does not mint a reference the nullable reader cannot discharge.
BEZEL_TEST(property_block_getter_closure_release_8763,
           nullable_block_getter_reference_not_leaked_200x_spec_28_9) {
    BEZEL_EXPECT_EQ(stdoutChecked(
        "module M; "
        "public class H "
        "  private f: () -> int; "
        "  public constructor() f := lambda(): int => 5; end; "
        "  public method Reset() f := lambda(): int => 6; end; "
        "  public property NP: (() -> int)? "
        "    get => do var spare := lambda(): int => 7; return f; end; "
        "  end; "
        "end H; "
        "public static class Program "
        "  public static method Main(args: Array<string>): int "
        "    var i := 0; "
        "    var tot := 0; "
        "    using var h := new H(); "
        "    while i < 200 do "
        "      var x := h.NP; "
        "      if x <> null then tot := tot + x!(); end; "
        "      h.Reset(); "
        "      i := i + 1; "
        "    end; "
        "    Console.Write(tot); "
        "    return 0; "
        "  end; "
        "end Program;"),
        "1199");
}

// fix #8763 (fix-forward 2) — the `?.` READ of a block getter.
//
// §7.5 (v0.359): "`x?.M` returns null if `x` is null; otherwise it performs
// the access. The result type is always nullable." The result of a `?.` read
// of a function-typed property is therefore a NULLABLE function value, and a
// nullable function value has no owner for a closure reference — the read
// sites that consume it (a binding local, a `??`, a discard, a call through
// the lifted value) have their own nullable protocol and none of them
// discharges the reference the getter handed over. So the `?.` spelling
// reads a BORROW (§28.9: "a getter never transfers a reclamation obligation
// to its reader") and its getter must not mint.
//
// MEASURED on the #8763 pair (621997773 + cf92fa30a) with `bezelc
// --sanitize=address` on this program (200 reads):
//   "SUMMARY: AddressSanitizer: 14328 byte(s) leaked in 398 allocation(s)"
// — one closure per read, on a shape the r30-c1 tip is CLEAN on.
//
// The row's getter body ALSO holds a closure local it does not return, so
// the row is red on the r30-c1 tip for that reason too (a function-typed
// getter's body was not analysed there) and green only when the body is
// analysed AND the `?.` read does not mint a reference nobody can discharge.
//
// The printed total is the program's own: 5 from the constructor's closure,
// then 6 per read after the first reset.
BEZEL_TEST(property_block_getter_closure_release_8763,
           safe_nav_block_getter_reference_not_leaked_200x_spec_28_9) {
    BEZEL_EXPECT_EQ(stdoutChecked(
        "module M; "
        "public class H "
        "  private f: () -> int; "
        "  public constructor() f := lambda(): int => 5; end; "
        "  public method Reset() f := lambda(): int => 6; end; "
        "  public property P: () -> int "
        "    get => do var spare := lambda(): int => 7; return f; end; "
        "  end; "
        "end H; "
        "public static class Program "
        "  public static method Main(args: Array<string>): int "
        "    var i := 0; "
        "    var tot := 0; "
        "    using var h := new H(); "
        "    var hn: H? := h; "
        "    while i < 200 do "
        "      var y := hn?.P; "
        "      if y <> null then tot := tot + y!(); end; "
        "      h.Reset(); "
        "      i := i + 1; "
        "    end; "
        "    Console.Write(tot); "
        "    return 0; "
        "  end; "
        "end Program;"),
        "1199");
}
