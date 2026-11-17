// Codegen — issue #8480: the #4422 / #4434 call-site release freed a
// reassigned closure local right after its first LEXICAL call, although the
// local was used again — the call site is inside a loop, or the local is
// returned or passed on after the call.
//
// THE RULE (§28.9, v0.357, "Runtime-managed types"): a function-typed value is
// runtime-managed — "the type's lifetime is managed by the runtime without
// user involvement". The value a closure local holds must therefore stay
// alive for as long as the local can still be used, however many times and in
// whatever way that is. §27.5 (v0.357) covers the rows whose lambda captures
// a variable. (§28.9 leaves the lifecycle MECHANISM — reference counting — to
// the implementation; the citation is for the observable guarantee.)
//
// ROOT CAUSE. A closure local the body assigns to was treated as "escaping"
// and got no scope-end release; instead `emitClosureCall` released the slot's
// reference right after the local's first call SITE (the #4422 / #4434
// discipline), on the premise that the single lexical call is the slot's
// last use. A count of call sites says nothing about that: the site runs
// again on the next loop iteration, and a `return` or an argument use can
// follow it.
//
// THE FIX. The slot of an assigned-to closure local owns one reference to
// whatever it holds at every point (the bind's, then each reassignment's), and
// `collectClosureReleaseEligible` now has that reference released at SCOPE
// END for every plain closure local it classifies, whatever the call count:
//   * the value never leaves the frame — `closureReleaseEligible_`, as a
//     never-reassigned local;
//   * the value can leave the frame (a `return` of the local, a field or
//     collection store) — the new `closureEscapingScopeEndRelease_`: released
//     at scope end with no capture-cell free, `return f;` mints the caller's
//     reference and the scope-end release stays on the return edge.
// The call-site release remains only as the fallback for a reassigned local
// the scope-end discipline does not cover.
//
// Every program loops 200 times and runs under generated-code
// AddressSanitizer AND LeakSanitizer (`runAotChecked`), with the full stdout
// compared against an expectation computed here, in C++, from the program's
// arithmetic.
//
// FAIL-WITHOUT, MEASURED on the pre-fix compiler (r30-c1 at 0cb32bb14,
// `bezelc --sanitize=address --link=system`, ASAN_OPTIONS=detect_leaks=1):
//
//   row                                                              pre-fix                                       with the fix
//   issue_repro_call_site_inside_loop_spec_28_9                      heap-use-after-free in main, 1 lines          clean, 201 lines
//   issue_repro_returned_after_its_call_spec_28_9                    heap-use-after-free in main, 1 lines          clean, 401 lines
//   issue_variant_reassigned_inside_the_loop_spec_28_9               heap-use-after-free in main, 1 lines          leak 800 B, 201 lines
//   reassigned_inside_the_loop_local_capture_spec_28_9               heap-use-after-free in main, 1 lines          clean, 201 lines
//   issue_variant_reassigned_back_to_another_local_spec_28_9         refcount over-release abort, 1 lines          leak 800 B, 201 lines
//   reassigned_back_to_another_local_local_capture_spec_28_9         refcount over-release abort, 1 lines          clean, 201 lines
//   issue_variant_alias_initialized_twin_spec_28_9                   refcount over-release abort, 1 lines          leak 800 B, 201 lines
//   alias_initialized_twin_local_capture_spec_28_9                   refcount over-release abort, 1 lines          clean, 201 lines
//   issue_variant_alias_twin_returned_after_its_call_spec_28_9       heap-use-after-free in main, 1 lines          clean, 401 lines
//   called_twice_then_returned_spec_28_9                             heap-use-after-free in bz$M.C$Pick, 0 lines   clean, 401 lines
//   called_once_then_passed_as_argument_spec_28_9                    heap-use-after-free in bz$M.C$Apply, 0 lines  clean, 201 lines
//   lambda_body_local_called_in_loop_spec_28_9                       heap-use-after-free in lam$0, 0 lines         clean, 201 lines
//   stored_into_field_then_reassigned_called_twice_spec_28_9         heap-use-after-free in main, 0 lines          clean, 201 lines
//   control_stored_into_field_then_reassigned_called_once_spec_28_9  clean, 201 lines                              clean, 201 lines
//   reassigned_capturing_local_stored_into_field_spec_27_5           heap-use-after-free in lam$0, 0 lines         clean, 201 lines
//   reassigned_alias_of_capturing_source_returned_spec_27_5          heap-use-after-free in lam$0, 3 lines         clean, 401 lines
//   capturing_local_reassigned_called_then_returned_spec_27_5        heap-use-after-free in main, 1 lines          clean, 401 lines
//
// ("N lines" is the stdout line count before the process ended. Every "leak"
// in the right-hand column predates the fix; the row that carries it says
// what leaks, and runs with LeakSanitizer off. The one row that is clean on
// both sides is the suite's boundary control; it says which wrong fix turns
// it red.)

#include <string>

#include "support/aot_support.hpp"   // runAotChecked — ASan/LSan AOT harness
#include "test_harness.hpp"

using bezel::test::runAotChecked;
using bezel::test::runAotCheckedLenient;

namespace {
// The program's stdout when it compiled, ran to exit code 0 and no sanitizer
// (ASan or LSan) fired; a marker otherwise, so a crash can never compare equal
// to an expectation.
std::string runCheckedOut(const std::string& prog) {
    auto r = runAotChecked(prog);
    if (!r.ran) return "<did-not-run>";
    if (r.exitCode != 0) return "<exit " + std::to_string(r.exitCode) + ">";
    return r.stdoutText;
}
// Generated-code ASan ON, LeakSanitizer OFF (issue #2214's lenient harness):
// only for the rows that say so, each of which names the unrelated,
// pre-existing leak it would otherwise trip on. A use-after-free, a
// double-free or the runtime's over-release abort still fails the row.
std::string runLenientOut(const std::string& prog) {
    auto r = runAotCheckedLenient(prog);
    if (!r.ran) return "<did-not-run>";
    if (r.exitCode != 0) return "<exit " + std::to_string(r.exitCode) + ">";
    return r.stdoutText;
}
void line(std::string& e, int v) { e += std::to_string(v) + "\n"; }
}  // namespace

// THE ISSUE'S FIRST REPRO, verbatim (200 iterations). `f` is reassigned once and
// then called at a single call SITE that a loop runs. Pre-fix: the call-site
// release freed the cell after the first iteration's call (heap-use-after-free
// in `main`, 1 line). 2 - 1 = 1.
BEZEL_TEST(closure_local_reassign_scope_end_release_8480,
           issue_repro_call_site_inside_loop_spec_28_9) {
    std::string e;
    for (int i = 0; i < 200; ++i) line(e, 1);
    e += "DONE\n";
    BEZEL_EXPECT_EQ(runCheckedOut(R"BZL(
module M;
public class C
  public static method Main(): int
    var f: (int, int) -> int := lambda(x: int, y: int) => x - y;
    var c := 1;
    if c > 0 then
      f := lambda(x: int, y: int) => y - x;
    end if;
    for var i in 0..<200 do
      Console.WriteLine(f(1, 2));
    end for;
    Console.WriteLine("DONE");
    return 0;
  end Main;
end C;
)BZL"), e);
}

// THE ISSUE'S SECOND REPRO, verbatim. `cmp` is reassigned (even iterations),
// called once, then RETURNED. Pre-fix: the call-site release dropped the
// reference the return then handed to the caller. Pick prints cmp(7, 2) +
// src(1, 1): -5 / 5; main prints h(1, 2) + h(4, 1): -2 / 2.
BEZEL_TEST(closure_local_reassign_scope_end_release_8480,
           issue_repro_returned_after_its_call_spec_28_9) {
    std::string e;
    for (int i = 0; i < 200; ++i) { line(e, i % 2 == 0 ? -5 : 5); line(e, i % 2 == 0 ? -2 : 2); }
    e += "DONE\n";
    BEZEL_EXPECT_EQ(runCheckedOut(R"BZL(
module M;
public class C
  public static method Pick(c: bool): (int, int) -> int
    var src: (int, int) -> int := lambda(x: int, y: int) => x - y;
    var cmp: (int, int) -> int := lambda(x: int, y: int) => x - y;
    if c then
      cmp := lambda(x: int, y: int) => y - x;
    end if;
    Console.WriteLine(cmp(7, 2) + src(1, 1));
    return cmp;
  end Pick;
  public static method Main(): int
    for var i in 0..<200 do
      var h := C.Pick(i mod 2 = 0);
      Console.WriteLine(h(1, 2) + h(4, 1));
    end for;
    Console.WriteLine("DONE");
    return 0;
  end Main;
end C;
)BZL"), e);
}

// THE ISSUE'S VARIANT (`p46`), verbatim: the reassignment is inside the loop as
// well. even: y - x + i = 1 + i; odd: the previous (even) iteration's lambda,
// whose captured `i` was i - 1: 1 + (i - 1). Pre-fix: heap-use-after-free in
// `main`, 1 line. LSan off for this row only: the lambda captures the FOR
// VARIABLE, whose capture cell nobody frees (4 B per lambda — one of the shapes
// #4522 records as deliberately declined); the next row is the same program
// capturing a plain local, under LSan.
BEZEL_TEST(closure_local_reassign_scope_end_release_8480,
           issue_variant_reassigned_inside_the_loop_spec_28_9) {
    std::string e;
    for (int i = 0; i < 200; ++i) line(e, i % 2 == 0 ? 1 + i : i);
    e += "DONE\n";
    BEZEL_EXPECT_EQ(runLenientOut(R"BZL(
module M;
public class C
  public static method Main(): int
    var cmp: (int, int) -> int := lambda(x: int, y: int) => x * y;
    for var i in 0..<200 do
      if i mod 2 = 0 then
        cmp := lambda(x: int, y: int) => y - x + i;
      end if;
      Console.WriteLine(cmp(1, 2));
    end for;
    Console.WriteLine("DONE");
    return 0;
  end Main;
end C;
)BZL"), e);
}

// The row above with the lambda capturing a per-iteration LOCAL copy of `i`:
// no leak to exempt, so LeakSanitizer is on.
BEZEL_TEST(closure_local_reassign_scope_end_release_8480,
           reassigned_inside_the_loop_local_capture_spec_28_9) {
    std::string e;
    for (int i = 0; i < 200; ++i) line(e, i % 2 == 0 ? 1 + i : i);
    e += "DONE\n";
    BEZEL_EXPECT_EQ(runCheckedOut(R"BZL(
module M;
public class C
  public static method Main(): int
    var cmp: (int, int) -> int := lambda(x: int, y: int) => x * y;
    for var i in 0..<200 do
      var ii: int := i;
      if i mod 2 = 0 then
        cmp := lambda(x: int, y: int) => y - x + ii;
      end if;
      Console.WriteLine(cmp(1, 2));
    end for;
    Console.WriteLine("DONE");
    return 0;
  end Main;
end C;
)BZL"), e);
}

// THE ISSUE'S VARIANT (`p38`), verbatim: the local is also reassigned back to
// another closure local. even: (1 + i) + 3; odd: g(1, 2) + g(4, 1) = 2. Pre-fix:
// the runtime's over-release guard fired (exit 134, 1 line). LSan off for this
// row only: the for-variable capture cell again (#4522).
BEZEL_TEST(closure_local_reassign_scope_end_release_8480,
           issue_variant_reassigned_back_to_another_local_spec_28_9) {
    std::string e;
    for (int i = 0; i < 200; ++i) line(e, i % 2 == 0 ? 4 + i : 2);
    e += "DONE\n";
    BEZEL_EXPECT_EQ(runLenientOut(R"BZL(
module M;
public class C
  public static method Main(): int
    var g: (int, int) -> int := lambda(x: int, y: int) => x - y;
    var cmp: (int, int) -> int := lambda(x: int, y: int) => x * y;
    for var i in 0..<200 do
      if i mod 2 = 0 then
        cmp := lambda(x: int, y: int) => y - x + i;
      else
        cmp := g;
      end if;
      Console.WriteLine(cmp(1, 2) + g(4, 1));
    end for;
    Console.WriteLine("DONE");
    return 0;
  end Main;
end C;
)BZL"), e);
}

// The row above capturing a per-iteration local copy of `i`, under LSan.
BEZEL_TEST(closure_local_reassign_scope_end_release_8480,
           reassigned_back_to_another_local_local_capture_spec_28_9) {
    std::string e;
    for (int i = 0; i < 200; ++i) line(e, i % 2 == 0 ? 4 + i : 2);
    e += "DONE\n";
    BEZEL_EXPECT_EQ(runCheckedOut(R"BZL(
module M;
public class C
  public static method Main(): int
    var g: (int, int) -> int := lambda(x: int, y: int) => x - y;
    var cmp: (int, int) -> int := lambda(x: int, y: int) => x * y;
    for var i in 0..<200 do
      var ii: int := i;
      if i mod 2 = 0 then
        cmp := lambda(x: int, y: int) => y - x + ii;
      else
        cmp := g;
      end if;
      Console.WriteLine(cmp(1, 2) + g(4, 1));
    end for;
    Console.WriteLine("DONE");
    return 0;
  end Main;
end C;
)BZL"), e);
}

// THE ISSUE'S ALIAS-INITIALIZED TWIN (`p35`), verbatim (`var cmp := g;`). Same
// arithmetic. Pre-fix: over-release guard, exit 134, 1 line. LSan off for this
// row only: the for-variable capture cell again (#4522).
BEZEL_TEST(closure_local_reassign_scope_end_release_8480,
           issue_variant_alias_initialized_twin_spec_28_9) {
    std::string e;
    for (int i = 0; i < 200; ++i) line(e, i % 2 == 0 ? 4 + i : 2);
    e += "DONE\n";
    BEZEL_EXPECT_EQ(runLenientOut(R"BZL(
module M;
public class C
  public static method Main(): int
    var g: (int, int) -> int := lambda(x: int, y: int) => x - y;
    var cmp: (int, int) -> int := g;
    for var i in 0..<200 do
      if i mod 2 = 0 then
        cmp := lambda(x: int, y: int) => y - x + i;
      else
        cmp := g;
      end if;
      Console.WriteLine(cmp(1, 2) + g(4, 1));
    end for;
    Console.WriteLine("DONE");
    return 0;
  end Main;
end C;
)BZL"), e);
}

// The row above capturing a per-iteration local copy of `i`, under LSan.
BEZEL_TEST(closure_local_reassign_scope_end_release_8480,
           alias_initialized_twin_local_capture_spec_28_9) {
    std::string e;
    for (int i = 0; i < 200; ++i) line(e, i % 2 == 0 ? 4 + i : 2);
    e += "DONE\n";
    BEZEL_EXPECT_EQ(runCheckedOut(R"BZL(
module M;
public class C
  public static method Main(): int
    var g: (int, int) -> int := lambda(x: int, y: int) => x - y;
    var cmp: (int, int) -> int := g;
    for var i in 0..<200 do
      var ii: int := i;
      if i mod 2 = 0 then
        cmp := lambda(x: int, y: int) => y - x + ii;
      else
        cmp := g;
      end if;
      Console.WriteLine(cmp(1, 2) + g(4, 1));
    end for;
    Console.WriteLine("DONE");
    return 0;
  end Main;
end C;
)BZL"), e);
}

// THE ISSUE'S ALIAS-INITIALIZED TWIN of the returned shape (`p61`), verbatim: on
// the odd iterations the returned value is the SOURCE's function value, whose
// own name `src` is released at scope end — the return must hand the caller a
// reference of its own.
BEZEL_TEST(closure_local_reassign_scope_end_release_8480,
           issue_variant_alias_twin_returned_after_its_call_spec_28_9) {
    std::string e;
    for (int i = 0; i < 200; ++i) { line(e, i % 2 == 0 ? -5 : 5); line(e, i % 2 == 0 ? -2 : 2); }
    e += "DONE\n";
    BEZEL_EXPECT_EQ(runCheckedOut(R"BZL(
module M;
public class C
  public static method Pick(c: bool): (int, int) -> int
    var src: (int, int) -> int := lambda(x: int, y: int) => x - y;
    var cmp: (int, int) -> int := src;
    if c then
      cmp := lambda(x: int, y: int) => y - x;
    end if;
    Console.WriteLine(cmp(7, 2) + src(1, 1));
    return cmp;
  end Pick;
  public static method Main(): int
    for var i in 0..<200 do
      var h := C.Pick(i mod 2 = 0);
      Console.WriteLine(h(1, 2) + h(4, 1));
    end for;
    Console.WriteLine("DONE");
    return 0;
  end Main;
end C;
)BZL"), e);
}

// Two calls, then the return. Pre-fix: heap-use-after-free in `main` (the
// scope-end release armed for a multi-call local fired on top of the return's
// transfer).
BEZEL_TEST(closure_local_reassign_scope_end_release_8480,
           called_twice_then_returned_spec_28_9) {
    std::string e;
    for (int i = 0; i < 200; ++i) { line(e, i % 2 == 0 ? -5 : 5); line(e, i % 2 == 0 ? -2 : 2); }
    e += "DONE\n";
    BEZEL_EXPECT_EQ(runCheckedOut(R"BZL(
module M;
public class C
  public static method Pick(c: bool): (int, int) -> int
    var cmp: (int, int) -> int := lambda(x: int, y: int) => x - y;
    if c then
      cmp := lambda(x: int, y: int) => y - x;
    end if;
    Console.WriteLine(cmp(7, 2) + cmp(1, 1));
    return cmp;
  end Pick;
  public static method Main(): int
    for var i in 0..<200 do
      var h := C.Pick(i mod 2 = 0);
      Console.WriteLine(h(1, 2) + h(4, 1));
    end for;
    Console.WriteLine("DONE");
    return 0;
  end Main;
end C;
)BZL"), e);
}

// A use after the single call that is NOT a call of the local: it is passed to
// another method. Pre-fix: heap-use-after-free. even: 1 + -2; odd: -1 + 2.
BEZEL_TEST(closure_local_reassign_scope_end_release_8480,
           called_once_then_passed_as_argument_spec_28_9) {
    std::string e;
    for (int i = 0; i < 200; ++i) line(e, i % 2 == 0 ? -1 : 1);
    e += "DONE\n";
    BEZEL_EXPECT_EQ(runCheckedOut(R"BZL(
module M;
public class C
  public static method Apply(f: (int, int) -> int): int
    return f(5, 3);
  end Apply;
  public static method Main(): int
    for var i in 0..<200 do
      var f: (int, int) -> int := lambda(x: int, y: int) => x - y;
      if i mod 2 = 0 then
        f := lambda(x: int, y: int) => y - x;
      end if;
      var a := f(1, 2);
      Console.WriteLine(a + C.Apply(f));
    end for;
    Console.WriteLine("DONE");
    return 0;
  end Main;
end C;
)BZL"), e);
}

// The same defect in a second venue: the reassigned local lives in a LAMBDA
// body. Pre-fix: heap-use-after-free.
BEZEL_TEST(closure_local_reassign_scope_end_release_8480,
           lambda_body_local_called_in_loop_spec_28_9) {
    std::string e;
    for (int i = 0; i < 200; ++i) line(e, i % 2 == 0 ? -3 : 3);
    e += "DONE\n";
    BEZEL_EXPECT_EQ(runCheckedOut(R"BZL(
module M;
public class C
  public static method Main(): int
    var run := lambda(n: int): int => do
      var f: (int, int) -> int := lambda(x: int, y: int) => x - y;
      if n > 0 then
        f := lambda(x: int, y: int) => y - x;
      end if;
      var t := 0;
      for var i in 0..<3 do
        t := t + f(1, 2);
      end for;
      return t;
    end;
    for var i in 0..<200 do
      Console.WriteLine(run(i mod 2));
    end for;
    Console.WriteLine("DONE");
    return 0;
  end Main;
end C;
)BZL"), e);
}

// The initial value leaves the frame through a field store BEFORE the
// reassignment; the reassigned local is then called twice. -1 + -3 + 0. Pre-fix:
// heap-use-after-free in `main`.
BEZEL_TEST(closure_local_reassign_scope_end_release_8480,
           stored_into_field_then_reassigned_called_twice_spec_28_9) {
    std::string e;
    for (int i = 0; i < 200; ++i) line(e, -4);
    e += "DONE\n";
    BEZEL_EXPECT_EQ(runCheckedOut(R"BZL(
module M;
public class H
  public F: (int, int) -> int := lambda(x: int, y: int) => x - y;
  public constructor() end constructor;
end H;
public class C
  public static method Main(): int
    using var h := new H();
    for var i in 0..<200 do
      var f: (int, int) -> int := lambda(x: int, y: int) => x - y;
      h.F := f;
      f := lambda(x: int, y: int) => y - x;
      Console.WriteLine(h.F(1, 2) + f(4, 1) + f(2, 2));
    end for;
    Console.WriteLine("DONE");
    return 0;
  end Main;
end C;
)BZL"), e);
}

// BOUNDARY CONTROL, the single-call twin of the row above: clean before and
// after. It goes RED under the obvious wrong fix — arming the scope-end
// release through `closureReleaseEligible_` alone, with no verdict for a
// local whose value leaves the frame: MEASURED on this change's first design,
// which did that, the stored closure's reference was dropped and the program
// leaked 64 B per iteration (12800 B).
BEZEL_TEST(closure_local_reassign_scope_end_release_8480,
           control_stored_into_field_then_reassigned_called_once_spec_28_9) {
    std::string e;
    for (int i = 0; i < 200; ++i) line(e, -4);
    e += "DONE\n";
    BEZEL_EXPECT_EQ(runCheckedOut(R"BZL(
module M;
public class H
  public F: (int, int) -> int := lambda(x: int, y: int) => x - y;
  public constructor() end constructor;
end H;
public class C
  public static method Main(): int
    using var h := new H();
    for var i in 0..<200 do
      var f: (int, int) -> int := lambda(x: int, y: int) => x - y;
      h.F := f;
      f := lambda(x: int, y: int) => y - x;
      Console.WriteLine(h.F(1, 2) + f(4, 1));
    end for;
    Console.WriteLine("DONE");
    return 0;
  end Main;
end C;
)BZL"), e);
}

// A reassigned, never-called local whose lambda CAPTURES a local, stored into a
// field that outlives the method. §27.5: the capture must stay readable for as
// long as the closure is. Pre-fix: the #4425 arm classified the local
// non-escaping, its capture cell was freed at scope end, and `h.F()` read it
// (heap-use-after-free in `lam$0`, 0 lines).
BEZEL_TEST(closure_local_reassign_scope_end_release_8480,
           reassigned_capturing_local_stored_into_field_spec_27_5) {
    std::string e;
    for (int i = 0; i < 200; ++i) line(e, 2 * (i + 1));
    e += "DONE\n";
    BEZEL_EXPECT_EQ(runCheckedOut(R"BZL(
module M;
public class H
  public F: () -> int := lambda() => 0;
  public constructor() end constructor;
  public method Set(k: int)
    var x := k;
    var f: () -> int := lambda() => x + 1;
    if k < 0 then
      f := lambda() => 2;
    end if;
    this.F := f;
  end Set;
end H;
public class C
  public static method Main(): int
    using var h := new H();
    for var i in 0..<200 do
      h.Set(i);
      Console.WriteLine(h.F() + h.F());
    end for;
    Console.WriteLine("DONE");
    return 0;
  end Main;
end C;
)BZL"), e);
}

// A reassigned alias is returned while (odd iterations) it still names its
// SOURCE's closure, which captures a local: the source's scope end must not
// free the capture the caller goes on reading. Mk prints g(1, 1) = k.
BEZEL_TEST(closure_local_reassign_scope_end_release_8480,
           reassigned_alias_of_capturing_source_returned_spec_27_5) {
    std::string e;
    for (int i = 0; i < 200; ++i) { line(e, i); line(e, i % 2 == 0 ? -2 : 2 + 2 * i); }
    e += "DONE\n";
    BEZEL_EXPECT_EQ(runCheckedOut(R"BZL(
module M;
public class C
  public static method Mk(k: int, c: bool): (int, int) -> int
    var kk: int := k;
    var g: (int, int) -> int := lambda(x: int, y: int) => x - y + kk;
    var cmp: (int, int) -> int := g;
    if c then
      cmp := lambda(x: int, y: int) => y - x;
    end if;
    Console.WriteLine(g(1, 1));
    return cmp;
  end Mk;
  public static method Main(): int
    for var i in 0..<200 do
      var h := C.Mk(i, i mod 2 = 0);
      Console.WriteLine(h(1, 2) + h(4, 1));
    end for;
    Console.WriteLine("DONE");
    return 0;
  end Main;
end C;
)BZL"), e);
}

// The returned shape with lambdas that CAPTURE a local: the returned closure's
// capture stays readable in the caller. Mk prints g(1, 1) = k; the caller
// prints (1 + k) + (-3 + k) on even iterations, (-1 + k) + (3 + k) on odd
// ones. Pre-fix: heap-use-after-free in `main`, 1 line.
BEZEL_TEST(closure_local_reassign_scope_end_release_8480,
           capturing_local_reassigned_called_then_returned_spec_27_5) {
    std::string e;
    for (int i = 0; i < 200; ++i) { line(e, i); line(e, i % 2 == 0 ? -2 + 2 * i : 2 + 2 * i); }
    e += "DONE\n";
    BEZEL_EXPECT_EQ(runCheckedOut(R"BZL(
module M;
public class C
  public static method Mk(k: int, c: bool): (int, int) -> int
    var kk: int := k;
    var g: (int, int) -> int := lambda(x: int, y: int) => x - y + kk;
    if c then
      g := lambda(x: int, y: int) => y - x + kk;
    end if;
    Console.WriteLine(g(1, 1));
    return g;
  end Mk;
  public static method Main(): int
    for var i in 0..<200 do
      var h := C.Mk(i, i mod 2 = 0);
      Console.WriteLine(h(1, 2) + h(4, 1));
    end for;
    Console.WriteLine("DONE");
    return 0;
  end Main;
end C;
)BZL"), e);
}
