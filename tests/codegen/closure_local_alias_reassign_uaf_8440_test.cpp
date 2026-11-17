// Codegen — issue #8440: a closure local bound from ANOTHER function value
// (`var cmp := g;`) and then reassigned released the cell its SOURCE still
// pointed at — a heap-use-after-free on the source's next call.
//
// THE RULE (§28.9, v0.357, "Runtime-managed types"): a function-typed value is
// runtime-managed — "the type's lifetime is managed by the runtime without
// user involvement". A program that binds, reassigns and calls function values
// therefore has no way to free one early, and none of the programs below may
// read freed memory. §27.5 (v0.357) adds, for the captured rows, that a
// modification of a captured variable inside a lambda is visible outside "and
// vice versa". (§28.9 leaves the lifecycle MECHANISM — reference counting —
// to the implementation; the citation is for the observable guarantee, not
// for the retain/release protocol.)
//
// ROOT CAUSE. `lowerLocalDecl` stored a borrow-initialized closure local with
// NO retain: `var cmp := g;` copied `g`'s cell pointer and owned nothing. But
// every site that later overwrites or retires such a slot treats it as
// holding +1:
//   * `lowerClosureLocalReassign` (the #4174 drop-on-overwrite) releases the
//     displaced value;
//   * `lowerCapturedClosureLocalReassign`, its captured-cell twin, does the
//     same through the capture cell;
//   * the #4422 call-site release in `emitClosureCall` releases the slot's
//     current value after the single lexical call of a reassigned local.
// So `cmp := lambda ...` dropped the reference `g` owned, the cell was freed,
// and `g(4, 1)` read it.
//
// THE FIX. A closure local whose initializer is a borrowed read (a local, a
// parameter, a field, a built-in collection element, or an if-expression over
// those) AND that is assigned to anywhere in its body takes its own +1 at the
// declaration (`closureAliasOwned_`), and is then tracked exactly like a
// producer-initialized local. No release site changed. What fix #8440 left
// out was filed, and fix #8479 / #8478 / #8480 closed most of it: an alias
// that is never assigned to now takes its own reference too (captured by a
// lambda — #8478; outliving an overwrite of its source — #8479), a reassigned
// local's reference is released at scope end instead of after its one lexical
// call (#8480), and the returned-alias leak (#8483) went with it. Function
// values read through property / indexer getters (#8481) remain.
//
// Every program loops 100 times (a one-shot repro is a vacuous oracle for a
// refcount defect) and runs under generated-code AddressSanitizer AND
// LeakSanitizer (`runAotChecked`), with the full stdout compared against an
// expectation computed here, in C++, from the program's arithmetic.
//
// FAIL-WITHOUT, MEASURED on the pre-fix compiler (r30-c1 at d60c087ca,
// `bezelc --sanitize=address`, ASAN_OPTIONS=detect_leaks=1):
//
//   row                                                            pre-fix                                     with the fix
//   issue_repro_conditionally_reassigned_alias_keeps_source_alive  heap-use-after-free in main, 1 lines        clean, 201 lines
//   unconditionally_reassigned_alias                               heap-use-after-free in main, 0 lines        clean, 101 lines
//   alias_never_called_after_reassign                              heap-use-after-free in main, 0 lines        clean, 101 lines
//   alias_called_twice_after_reassign                              heap-use-after-free in main, 0 lines        clean, 101 lines
//   alias_with_inferred_type                                       heap-use-after-free in main, 0 lines        clean, 101 lines
//   alias_of_parameter                                             heap-use-after-free in bz$M.C$Run, 0 lines  leak 64 B, 101 lines
//   alias_of_field                                                 heap-use-after-free in main, 0 lines        clean, 101 lines
//   alias_of_array_element                                         heap-use-after-free in main, 0 lines        leak 272 B, 101 lines
//   alias_of_if_expression_over_locals                             heap-use-after-free in main, 0 lines        clean, 101 lines
//   alias_of_alias                                                 heap-use-after-free in main, 0 lines        leak 64 B (intermittent), 101 lines
//   alias_of_method_reference_local                                heap-use-after-free in main, 0 lines        clean, 101 lines
//   source_captures_value                                          heap-use-after-free in main, 0 lines        clean, 101 lines
//   unconditional_reassign_source_captures_class                   heap-use-after-free in main, 0 lines        clean, 101 lines
//   unconditional_reassign_alias_of_field                          heap-use-after-free in main, 0 lines        clean, 101 lines
//   source_captures_string                                         heap-use-after-free in main, 0 lines        leak 2195 B, 101 lines
//   source_captures_class                                          heap-use-after-free in main, 0 lines        clean, 101 lines
//   source_captures_owned_parameter                                heap-use-after-free in bz$M.C$Run, 0 lines  leak 800 B, 101 lines
//   reassign_inside_try_then_call_in_catch                         heap-use-after-free in main, 1 lines        clean, 201 lines
//   reassign_inside_case_arms                                      heap-use-after-free in main, 0 lines        clean, 134 lines
//   reassign_inside_while_do_try_finally                           heap-use-after-free in main, 0 lines        clean, 101 lines
//   alias_passed_as_argument_then_reassigned                       heap-use-after-free in main, 0 lines        clean, 101 lines
//   captured_alias_reassigned_inside_lambda                        heap-use-after-free in main, 0 lines        clean, 101 lines
//   captured_alias_reassigned_outside_lambda                       heap-use-after-free in main, 0 lines        clean, 101 lines
//   alias_declared_in_lambda_body                                  heap-use-after-free in lam$1, 0 lines       clean, 101 lines
//   control_returned_reassigned_alias_is_live                      leak 6400 B, 101 lines                      leak 6400 B, 101 lines
//   alias_reassigned_to_lambda_then_back_to_source                 double-free in free, 0 lines                clean, 101 lines
//   control_alias_never_reassigned                                 leak 64 B, 101 lines                        leak 64 B, 101 lines
//   control_producer_local_reassigned_to_source                    clean, 101 lines                            clean, 101 lines
//
// ("N lines" is the stdout line count before the process ended; a full run
// prints 101, 134 or 201. Every "leak" in the right-hand column predates the
// fix; the row that carries it says what leaks, and runs with LeakSanitizer
// off. The three `control_` rows pass before and after.)
//
// SINCE fix #8479 / #8480 (the table above is fix #8440's own measurement):
// `alias_of_alias`, `control_alias_never_reassigned` (leak 64 B) and
// `control_returned_reassigned_alias_is_live` (leak 6400 B) are CLEAN, and
// those three rows now run under LeakSanitizer like the rest.

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
// pre-existing leak it would otherwise trip on. A use-after-free or a
// double-free still fails the row.
std::string runLenientOut(const std::string& prog) {
    auto r = runAotCheckedLenient(prog);
    if (!r.ran) return "<did-not-run>";
    if (r.exitCode != 0) return "<exit " + std::to_string(r.exitCode) + ">";
    return r.stdoutText;
}
void line(std::string& e, int v) { e += std::to_string(v) + "\n"; }
}  // namespace

// THE ISSUE'S OWN REPRO, verbatim. `cmp` aliases `g`, is reassigned on the even
// iterations, and `g` is called afterwards. Pre-fix: the reassignment released
// `g`'s only reference and `g(4, 1)` read the freed cell (heap-use-after-free in
// `main`, one line printed). The issue text says "100 lines"; the loop prints two
// lines per iteration, so the full output is 200 lines and DONE.
BEZEL_TEST(closure_local_alias_reassign_uaf_8440,
           issue_repro_conditionally_reassigned_alias_keeps_source_alive_spec_28_9) {
    std::string e;
    for (int i = 0; i < 100; ++i) { line(e, i % 2 == 0 ? 1 : -1); line(e, 6); }
    e += "DONE\n";
    BEZEL_EXPECT_EQ(runCheckedOut(R"BZL(
module M;
public class C
  public static method Main(): int
    var g: (int, int) -> int := lambda(x: int, y: int) => x - y;
    for var i in 0..<100 do
      var cmp: (int, int) -> int := g;
      if i mod 2 = 0 then
        cmp := lambda(x: int, y: int) => y - x;
      end if;
      var a: Array<int> := [3, 1, 2];
      Console.WriteLine(cmp(1, 2));
      Console.WriteLine(a[0] + g(4, 1));
    end for;
    Console.WriteLine("DONE");
    return 0;
  end Main;
end C;
)BZL"), e);
}

// Reassignment on EVERY iteration (no branch): cmp(1,2) = 1 via the new lambda,
// g(4,1) = 3.
BEZEL_TEST(closure_local_alias_reassign_uaf_8440,
           unconditionally_reassigned_alias_spec_28_9) {
    std::string e;
    for (int i = 0; i < 100; ++i) { line(e, 4); }
    e += "DONE\n";
    BEZEL_EXPECT_EQ(runCheckedOut(R"BZL(
module M;
public class C
  public static method Main(): int
    var g: (int, int) -> int := lambda(x: int, y: int) => x - y;
    for var i in 0..<100 do
      var cmp: (int, int) -> int := g;
      cmp := lambda(x: int, y: int) => y - x;
      Console.WriteLine(cmp(1, 2) + g(4, 1));
    end for;
    Console.WriteLine("DONE");
    return 0;
  end Main;
end C;
)BZL"), e);
}

// Call-after-reassign axis, ZERO calls of the alias: the #4425 override arms the
// alias's scope-end release, which must release the alias's own reference.
BEZEL_TEST(closure_local_alias_reassign_uaf_8440,
           alias_never_called_after_reassign_spec_28_9) {
    std::string e;
    for (int i = 0; i < 100; ++i) { line(e, 3); }
    e += "DONE\n";
    BEZEL_EXPECT_EQ(runCheckedOut(R"BZL(
module M;
public class C
  public static method Main(): int
    var g: (int, int) -> int := lambda(x: int, y: int) => x - y;
    for var i in 0..<100 do
      var cmp: (int, int) -> int := g;
      if i mod 2 = 0 then
        cmp := lambda(x: int, y: int) => y - x;
      end if;
      Console.WriteLine(g(4, 1));
    end for;
    Console.WriteLine("DONE");
    return 0;
  end Main;
end C;
)BZL"), e);
}

// Call-after-reassign axis, TWO calls of the alias (the #4422 multi-call arm:
// scope-end release, no call-site release). even: 1 + -3 + 3; odd: -1 + 3 + 3.
BEZEL_TEST(closure_local_alias_reassign_uaf_8440,
           alias_called_twice_after_reassign_spec_28_9) {
    std::string e;
    for (int i = 0; i < 100; ++i) { line(e, i % 2 == 0 ? 1 : 5); }
    e += "DONE\n";
    BEZEL_EXPECT_EQ(runCheckedOut(R"BZL(
module M;
public class C
  public static method Main(): int
    var g: (int, int) -> int := lambda(x: int, y: int) => x - y;
    for var i in 0..<100 do
      var cmp: (int, int) -> int := g;
      if i mod 2 = 0 then
        cmp := lambda(x: int, y: int) => y - x;
      end if;
      Console.WriteLine(cmp(1, 2) + cmp(5, 2) + g(4, 1));
    end for;
    Console.WriteLine("DONE");
    return 0;
  end Main;
end C;
)BZL"), e);
}

// Alias form: `var cmp := g;` with the function type INFERRED.
BEZEL_TEST(closure_local_alias_reassign_uaf_8440,
           alias_with_inferred_type_spec_28_9) {
    std::string e;
    for (int i = 0; i < 100; ++i) { line(e, i % 2 == 0 ? 4 : 2); }
    e += "DONE\n";
    BEZEL_EXPECT_EQ(runCheckedOut(R"BZL(
module M;
public class C
  public static method Main(): int
    var g: (int, int) -> int := lambda(x: int, y: int) => x - y;
    for var i in 0..<100 do
      var cmp := g;
      if i mod 2 = 0 then
        cmp := lambda(x: int, y: int) => y - x;
      end if;
      Console.WriteLine(cmp(1, 2) + g(4, 1));
    end for;
    Console.WriteLine("DONE");
    return 0;
  end Main;
end C;
)BZL"), e);
}

// Alias form: the source is a by-value function-typed PARAMETER. Pre-fix the
// callee released the CALLER's reference (use-after-free in `Run`).
// LSan off for this row only: the program leaks `g`'s own cell ONCE (64 B, 1
// object, allocated in `main`) with or without an alias — measured identical on
// the pre-fix compiler for the alias-free control (`cmp` initialized from a
// lambda; tracked as #8482). Generated-code ASan stays on, which is the axis
// this row tests.
BEZEL_TEST(closure_local_alias_reassign_uaf_8440,
           alias_of_parameter_spec_28_9) {
    std::string e;
    for (int i = 0; i < 100; ++i) { line(e, i % 2 == 0 ? 13 : 11); }
    e += "DONE\n";
    BEZEL_EXPECT_EQ(runLenientOut(R"BZL(
module M;
public class C
  public static method Run(f: (int, int) -> int, i: int): int
    var cmp: (int, int) -> int := f;
    if i mod 2 = 0 then
      cmp := lambda(x: int, y: int) => y - x;
    end if;
    return cmp(1, 2) + f(4, 1);
  end Run;
  public static method Main(): int
    var g: (int, int) -> int := lambda(x: int, y: int) => x - y;
    for var i in 0..<100 do
      Console.WriteLine(C.Run(g, i) + g(10, 1));
    end for;
    Console.WriteLine("DONE");
    return 0;
  end Main;
end C;
)BZL"), e);
}

// Alias form: the source is a function-typed FIELD of another object.
BEZEL_TEST(closure_local_alias_reassign_uaf_8440,
           alias_of_field_spec_28_9) {
    std::string e;
    for (int i = 0; i < 100; ++i) { line(e, i % 2 == 0 ? 4 : 2); }
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
    for var i in 0..<100 do
      var cmp: (int, int) -> int := h.F;
      if i mod 2 = 0 then
        cmp := lambda(x: int, y: int) => y - x;
      end if;
      Console.WriteLine(cmp(1, 2) + h.F(4, 1));
    end for;
    Console.WriteLine("DONE");
    return 0;
  end Main;
end C;
)BZL"), e);
}

// Alias form: the source is an ELEMENT of an `Array` of function values.
// LSan off for this row only: the `Array` of function values itself is reported
// leaked once at exit (272 B) with or without an alias — the alias-free control
// compiles to a byte-identical executable before and after the fix (#6212).
BEZEL_TEST(closure_local_alias_reassign_uaf_8440,
           alias_of_array_element_spec_28_9) {
    std::string e;
    for (int i = 0; i < 100; ++i) { line(e, i % 2 == 0 ? 4 : 2); }
    e += "DONE\n";
    BEZEL_EXPECT_EQ(runLenientOut(R"BZL(
module M;
public class C
  public static method Main(): int
    var fs: Array<(int, int) -> int> := [lambda(x: int, y: int) => x - y];
    for var i in 0..<100 do
      var cmp: (int, int) -> int := fs[0];
      if i mod 2 = 0 then
        cmp := lambda(x: int, y: int) => y - x;
      end if;
      Console.WriteLine(cmp(1, 2) + fs[0](4, 1));
    end for;
    Console.WriteLine("DONE");
    return 0;
  end Main;
end C;
)BZL"), e);
}

// Alias form: the source is an if-expression selecting between two locals.
// even: 1 + 3 + 2; odd and i mod 3 = 0: -1 + 3 + 2; other odd: 3 + 3 + 2.
BEZEL_TEST(closure_local_alias_reassign_uaf_8440,
           alias_of_if_expression_over_locals_spec_28_9) {
    std::string e;
    for (int i = 0; i < 100; ++i) { line(e, i % 2 == 0 ? 6 : (i % 3 == 0 ? 4 : 8)); }
    e += "DONE\n";
    BEZEL_EXPECT_EQ(runCheckedOut(R"BZL(
module M;
public class C
  public static method Main(): int
    var g: (int, int) -> int := lambda(x: int, y: int) => x - y;
    var h: (int, int) -> int := lambda(x: int, y: int) => x + y;
    for var i in 0..<100 do
      var cmp: (int, int) -> int := (if i mod 3 = 0 then g else h);
      if i mod 2 = 0 then
        cmp := lambda(x: int, y: int) => y - x;
      end if;
      Console.WriteLine(cmp(1, 2) + g(4, 1) + h(1, 1));
    end for;
    Console.WriteLine("DONE");
    return 0;
  end Main;
end C;
)BZL"), e);
}

// Alias form: an alias of an alias (`a := g; b := a; b := lambda`); all three
// names are called after the reassignment.
// Under LeakSanitizer since fix #8479: `a`, an alias that is never reassigned,
// used to keep `g` from being released at scope end (64 B, once); it now owns
// its own reference and both are released.
BEZEL_TEST(closure_local_alias_reassign_uaf_8440,
           alias_of_alias_spec_28_9) {
    std::string e;
    for (int i = 0; i < 100; ++i) { line(e, i % 2 == 0 ? 12 : 10); }
    e += "DONE\n";
    BEZEL_EXPECT_EQ(runCheckedOut(R"BZL(
module M;
public class C
  public static method Main(): int
    var g: (int, int) -> int := lambda(x: int, y: int) => x - y;
    for var i in 0..<100 do
      var a: (int, int) -> int := g;
      var b: (int, int) -> int := a;
      if i mod 2 = 0 then
        b := lambda(x: int, y: int) => y - x;
      end if;
      Console.WriteLine(b(1, 2) + a(4, 1) + g(9, 1));
    end for;
    Console.WriteLine("DONE");
    return 0;
  end Main;
end C;
)BZL"), e);
}

// Source kind: `g` is a METHOD REFERENCE cell (§27.6), not a lambda.
BEZEL_TEST(closure_local_alias_reassign_uaf_8440,
           alias_of_method_reference_local_spec_28_9) {
    std::string e;
    for (int i = 0; i < 100; ++i) { line(e, i % 2 == 0 ? 4 : 2); }
    e += "DONE\n";
    BEZEL_EXPECT_EQ(runCheckedOut(R"BZL(
module M;
public class C
  public static method Sub(x: int, y: int): int => x - y;
  public static method Main(): int
    var g: (int, int) -> int := C.Sub;
    for var i in 0..<100 do
      var cmp: (int, int) -> int := g;
      if i mod 2 = 0 then
        cmp := lambda(x: int, y: int) => y - x;
      end if;
      Console.WriteLine(cmp(1, 2) + g(4, 1));
    end for;
    Console.WriteLine("DONE");
    return 0;
  end Main;
end C;
)BZL"), e);
}

// Capture kind: the source closure and the replacement both capture a VALUE
// local (`k` = 7). even: (1 + 7) + 10; odd: 6 + 10.
BEZEL_TEST(closure_local_alias_reassign_uaf_8440,
           source_captures_value_spec_27_5_28_9) {
    std::string e;
    for (int i = 0; i < 100; ++i) { line(e, i % 2 == 0 ? 18 : 16); }
    e += "DONE\n";
    BEZEL_EXPECT_EQ(runCheckedOut(R"BZL(
module M;
public class C
  public static method Main(): int
    var k := 7;
    var g: (int, int) -> int := lambda(x: int, y: int) => x - y + k;
    for var i in 0..<100 do
      var cmp: (int, int) -> int := g;
      if i mod 2 = 0 then
        cmp := lambda(x: int, y: int) => y - x + k;
      end if;
      Console.WriteLine(cmp(1, 2) + g(4, 1));
    end for;
    Console.WriteLine("DONE");
    return 0;
  end Main;
end C;
)BZL"), e);
}

// Unconditional reassignment x CLASS captures.
BEZEL_TEST(closure_local_alias_reassign_uaf_8440,
           unconditional_reassign_source_captures_class_spec_27_5_28_9) {
    std::string e;
    for (int i = 0; i < 100; ++i) { line(e, 14); }
    e += "DONE\n";
    BEZEL_EXPECT_EQ(runCheckedOut(R"BZL(
module M;
public class Box
  public N: int := 5;
  public constructor() end constructor;
end Box;
public class C
  public static method Main(): int
    using var bx := new Box();
    var g: (int, int) -> int := lambda(x: int, y: int) => x - y + bx.N;
    for var i in 0..<100 do
      using var b2 := new Box();
      var cmp: (int, int) -> int := g;
      cmp := lambda(x: int, y: int) => y - x + b2.N;
      Console.WriteLine(cmp(1, 2) + g(4, 1));
    end for;
    Console.WriteLine("DONE");
    return 0;
  end Main;
end C;
)BZL"), e);
}

// Unconditional reassignment x FIELD source.
BEZEL_TEST(closure_local_alias_reassign_uaf_8440,
           unconditional_reassign_alias_of_field_spec_28_9) {
    std::string e;
    for (int i = 0; i < 100; ++i) { line(e, 4); }
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
    for var i in 0..<100 do
      var cmp: (int, int) -> int := h.F;
      cmp := lambda(x: int, y: int) => y - x;
      Console.WriteLine(cmp(1, 2) + h.F(4, 1));
    end for;
    Console.WriteLine("DONE");
    return 0;
  end Main;
end C;
)BZL"), e);
}

// Capture kind: STRING captures (`s` is "ab3"; `t` is "q" + i). even:
// (1 + t.Length) + 6; odd: 2 + 6.
// LSan off for this row only: the STRING capture cells leak 2195 B per run with
// or without an alias — measured identical on the pre-fix compiler for the
// alias-free control (the contents-managed capture-cell residual, #4522).
BEZEL_TEST(closure_local_alias_reassign_uaf_8440,
           source_captures_string_spec_27_5_28_9) {
    std::string e;
    for (int i = 0; i < 100; ++i) { line(e, i % 2 == 0 ? (i < 10 ? 9 : 10) : 8); }
    e += "DONE\n";
    BEZEL_EXPECT_EQ(runLenientOut(R"BZL(
module M;
public class C
  public static method Main(): int
    var s := "ab" + 3.ToString();
    var g: (int, int) -> int := lambda(x: int, y: int) => x - y + s.Length;
    for var i in 0..<100 do
      var t := "q" + i.ToString();
      var cmp: (int, int) -> int := g;
      if i mod 2 = 0 then
        cmp := lambda(x: int, y: int) => y - x + t.Length;
      end if;
      Console.WriteLine(cmp(1, 2) + g(4, 1));
    end for;
    Console.WriteLine("DONE");
    return 0;
  end Main;
end C;
)BZL"), e);
}

// Capture kind: CLASS captures (a `using var` object borrowed by the closure).
BEZEL_TEST(closure_local_alias_reassign_uaf_8440,
           source_captures_class_spec_27_5_28_9) {
    std::string e;
    for (int i = 0; i < 100; ++i) { line(e, i % 2 == 0 ? 14 : 12); }
    e += "DONE\n";
    BEZEL_EXPECT_EQ(runCheckedOut(R"BZL(
module M;
public class Box
  public N: int := 5;
  public constructor() end constructor;
end Box;
public class C
  public static method Main(): int
    using var bx := new Box();
    var g: (int, int) -> int := lambda(x: int, y: int) => x - y + bx.N;
    for var i in 0..<100 do
      using var b2 := new Box();
      var cmp: (int, int) -> int := g;
      if i mod 2 = 0 then
        cmp := lambda(x: int, y: int) => y - x + b2.N;
      end if;
      Console.WriteLine(cmp(1, 2) + g(4, 1));
    end for;
    Console.WriteLine("DONE");
    return 0;
  end Main;
end C;
)BZL"), e);
}

// Capture kind: an OWNED parameter captured by both closures.
// LSan off for this row only: the captured PARAMETER's cell leaks 8 B per call
// (800 B) with or without an alias — measured identical on the pre-fix compiler
// for the alias-free control (the parameter capture-cell residual, #4522).
BEZEL_TEST(closure_local_alias_reassign_uaf_8440,
           source_captures_owned_parameter_spec_27_5_28_9) {
    std::string e;
    for (int i = 0; i < 100; ++i) { line(e, i % 2 == 0 ? 14 : 12); }
    e += "DONE\n";
    BEZEL_EXPECT_EQ(runLenientOut(R"BZL(
module M;
public class Box
  public N: int := 5;
  public constructor() end constructor;
end Box;
public class C
  public static method Run(owned bx: Box, i: int): int
    var g: (int, int) -> int := lambda(x: int, y: int) => x - y + bx.N;
    var cmp: (int, int) -> int := g;
    if i mod 2 = 0 then
      cmp := lambda(x: int, y: int) => y - x + bx.N;
    end if;
    return cmp(1, 2) + g(4, 1);
  end Run;
  public static method Main(): int
    for var i in 0..<100 do
      Console.WriteLine(C.Run(new Box(), i));
    end for;
    Console.WriteLine("DONE");
    return 0;
  end Main;
end C;
)BZL"), e);
}

// Reassignment inside a `try` body, the alias called on BOTH the normal and the
// catch path.
BEZEL_TEST(closure_local_alias_reassign_uaf_8440,
           reassign_inside_try_then_call_in_catch_spec_28_9) {
    std::string e;
    for (int i = 0; i < 100; ++i) { line(e, i % 5 == 0 ? (i % 2 == 0 ? -1 : 1) : (i % 2 == 0 ? 1 : -1)); line(e, 3); }
    e += "DONE\n";
    BEZEL_EXPECT_EQ(runCheckedOut(R"BZL(
module M;
public class C
  public static method Main(): int
    var g: (int, int) -> int := lambda(x: int, y: int) => x - y;
    for var i in 0..<100 do
      var cmp: (int, int) -> int := g;
      try
        if i mod 2 = 0 then
          cmp := lambda(x: int, y: int) => y - x;
        end if;
        if i mod 5 = 0 then
          throw new InvalidOperationException("x");
        end if;
        Console.WriteLine(cmp(1, 2));
      catch
        InvalidOperationException => Console.WriteLine(cmp(2, 1)),
      end try;
      Console.WriteLine(g(4, 1));
    end for;
    Console.WriteLine("DONE");
    return 0;
  end Main;
end C;
)BZL"), e);
}

// Reassignment inside `case` arms (a `do` block arm and a single-statement arm).
BEZEL_TEST(closure_local_alias_reassign_uaf_8440,
           reassign_inside_case_arms_spec_28_9) {
    std::string e;
    for (int i = 0; i < 100; ++i) { if (i % 3 == 2) e += "keep\n"; line(e, i % 3 == 0 ? 4 : (i % 3 == 1 ? 6 : 2)); }
    e += "DONE\n";
    BEZEL_EXPECT_EQ(runCheckedOut(R"BZL(
module M;
public class C
  public static method Main(): int
    var g: (int, int) -> int := lambda(x: int, y: int) => x - y;
    for var i in 0..<100 do
      var cmp: (int, int) -> int := g;
      case i mod 3 of
        0 => do cmp := lambda(x: int, y: int) => y - x; end,
        1 => cmp := lambda(x: int, y: int) => x + y,
        _ => Console.WriteLine("keep")
      end case;
      Console.WriteLine(cmp(1, 2) + g(4, 1));
    end for;
    Console.WriteLine("DONE");
    return 0;
  end Main;
end C;
)BZL"), e);
}

// Reassignment nested in while / do / try / finally, the alias called twice.
BEZEL_TEST(closure_local_alias_reassign_uaf_8440,
           reassign_inside_while_do_try_finally_spec_28_9) {
    std::string e;
    for (int i = 0; i < 100; ++i) { line(e, i % 5 == 0 ? 9 : 3); }
    e += "DONE\n";
    BEZEL_EXPECT_EQ(runCheckedOut(R"BZL(
module M;
public class C
  public static method Main(): int
    var g: (int, int) -> int := lambda(x: int, y: int) => x - y;
    var i := 0;
    while i < 100 do
      var cmp: (int, int) -> int := g;
      do
        try
          if i mod 2 = 0 then
            cmp := lambda(x: int, y: int) => y - x;
          end if;
        finally
          if i mod 5 = 0 then
            cmp := lambda(x: int, y: int) => x + y;
          end if;
        end try;
      end;
      Console.WriteLine(cmp(1, 2) + cmp(2, 1) + g(4, 1));
      i := i + 1;
    end while;
    Console.WriteLine("DONE");
    return 0;
  end Main;
end C;
)BZL"), e);
}

// The alias is passed to a call-bounded parameter (§28.14) before and after the
// reassignment.
BEZEL_TEST(closure_local_alias_reassign_uaf_8440,
           alias_passed_as_argument_then_reassigned_spec_28_9) {
    std::string e;
    for (int i = 0; i < 100; ++i) { line(e, i % 2 == 0 ? 3 : 7); }
    e += "DONE\n";
    BEZEL_EXPECT_EQ(runCheckedOut(R"BZL(
module M;
public class C
  public static method Apply(f: (int, int) -> int): int
    return f(5, 3);
  end Apply;
  public static method Main(): int
    var g: (int, int) -> int := lambda(x: int, y: int) => x - y;
    for var i in 0..<100 do
      var cmp: (int, int) -> int := g;
      var a := C.Apply(cmp);
      if i mod 2 = 0 then
        cmp := lambda(x: int, y: int) => y - x;
      end if;
      Console.WriteLine(a + C.Apply(cmp) + g(4, 1));
    end for;
    Console.WriteLine("DONE");
    return 0;
  end Main;
end C;
)BZL"), e);
}

// The captured-cell twin (`lowerCapturedClosureLocalReassign`): the alias is
// captured by a lambda that REASSIGNS it (§27.5: a modification inside the
// lambda is visible outside).
BEZEL_TEST(closure_local_alias_reassign_uaf_8440,
           captured_alias_reassigned_inside_lambda_spec_27_5_28_9) {
    std::string e;
    for (int i = 0; i < 100; ++i) { line(e, i % 2 == 0 ? 4 : 2); }
    e += "DONE\n";
    BEZEL_EXPECT_EQ(runCheckedOut(R"BZL(
module M;
public class C
  public static method Main(): int
    var g: (int, int) -> int := lambda(x: int, y: int) => x - y;
    for var i in 0..<100 do
      var cmp: (int, int) -> int := g;
      var mut := lambda() => do
        cmp := lambda(x: int, y: int) => y - x;
      end;
      if i mod 2 = 0 then
        mut();
      end if;
      Console.WriteLine(cmp(1, 2) + g(4, 1));
    end for;
    Console.WriteLine("DONE");
    return 0;
  end Main;
end C;
)BZL"), e);
}

// The captured-cell twin: the alias is captured by a READER lambda and
// reassigned by the enclosing body.
BEZEL_TEST(closure_local_alias_reassign_uaf_8440,
           captured_alias_reassigned_outside_lambda_spec_27_5_28_9) {
    std::string e;
    for (int i = 0; i < 100; ++i) { line(e, i % 2 == 0 ? 4 : 2); }
    e += "DONE\n";
    BEZEL_EXPECT_EQ(runCheckedOut(R"BZL(
module M;
public class C
  public static method Main(): int
    var g: (int, int) -> int := lambda(x: int, y: int) => x - y;
    for var i in 0..<100 do
      var cmp: (int, int) -> int := g;
      var rd := lambda() => cmp(1, 2);
      if i mod 2 = 0 then
        cmp := lambda(x: int, y: int) => y - x;
      end if;
      Console.WriteLine(rd() + g(4, 1));
    end for;
    Console.WriteLine("DONE");
    return 0;
  end Main;
end C;
)BZL"), e);
}

// The alias is declared INSIDE a block lambda, bound from a captured local, and
// reassigned there.
BEZEL_TEST(closure_local_alias_reassign_uaf_8440,
           alias_declared_in_lambda_body_spec_27_5_28_9) {
    std::string e;
    for (int i = 0; i < 100; ++i) { line(e, i % 2 == 0 ? 10 : 8); }
    e += "DONE\n";
    BEZEL_EXPECT_EQ(runCheckedOut(R"BZL(
module M;
public class C
  public static method Main(): int
    var g: (int, int) -> int := lambda(x: int, y: int) => x - y;
    var run := lambda(i: int): int => do
      var cmp: (int, int) -> int := g;
      if i mod 2 = 0 then
        cmp := lambda(x: int, y: int) => y - x;
      end if;
      return cmp(1, 2) + g(4, 1);
    end;
    for var j in 0..<100 do
      Console.WriteLine(run(j) + g(7, 1));
    end for;
    Console.WriteLine("DONE");
    return 0;
  end Main;
end C;
)BZL"), e);
}

// CONTROL (passes before and after): the reassigned alias is RETURNED from the
// method and the caller calls it twice.
// Under LeakSanitizer since fix #8480: the return minted the caller's reference
// on top of the alias's own, which nothing released (one cell per call,
// 6400 B — #8483). The alias's reference is now released at scope end on every
// path, the returning one included. The row pins that the returned value is
// LIVE and that nothing leaks.
BEZEL_TEST(closure_local_alias_reassign_uaf_8440,
           control_returned_reassigned_alias_is_live_spec_28_9) {
    std::string e;
    for (int i = 0; i < 100; ++i) { line(e, i % 2 == 0 ? -2 : 2); }
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
    return cmp;
  end Pick;
  public static method Main(): int
    for var i in 0..<100 do
      var h := C.Pick(i mod 2 = 0);
      Console.WriteLine(h(1, 2) + h(4, 1));
    end for;
    Console.WriteLine("DONE");
    return 0;
  end Main;
end C;
)BZL"), e);
}

// A chain: alias, reassigned to a lambda, reassigned BACK to the source, then
// conditionally to another lambda. Pre-fix: a double-free.
BEZEL_TEST(closure_local_alias_reassign_uaf_8440,
           alias_reassigned_to_lambda_then_back_to_source_spec_28_9) {
    std::string e;
    for (int i = 0; i < 100; ++i) { line(e, i % 3 == 0 ? 6 : 2); }
    e += "DONE\n";
    BEZEL_EXPECT_EQ(runCheckedOut(R"BZL(
module M;
public class C
  public static method Main(): int
    var g: (int, int) -> int := lambda(x: int, y: int) => x - y;
    for var i in 0..<100 do
      var cmp: (int, int) -> int := g;
      cmp := lambda(x: int, y: int) => y - x;
      cmp := g;
      if i mod 3 = 0 then
        cmp := lambda(x: int, y: int) => x + y;
      end if;
      Console.WriteLine(cmp(1, 2) + g(4, 1));
    end for;
    Console.WriteLine("DONE");
    return 0;
  end Main;
end C;
)BZL"), e);
}

// CONTROL: an alias that is never reassigned was not in scope of fix #8440.
// Under LeakSanitizer since fix #8479: such an alias used to keep its source
// from being released at scope end (64 B, once); it now owns a reference of its
// own, and both are released.
BEZEL_TEST(closure_local_alias_reassign_uaf_8440,
           control_alias_never_reassigned_spec_28_9) {
    std::string e;
    for (int i = 0; i < 100; ++i) { line(e, 2); }
    e += "DONE\n";
    BEZEL_EXPECT_EQ(runCheckedOut(R"BZL(
module M;
public class C
  public static method Main(): int
    var g: (int, int) -> int := lambda(x: int, y: int) => x - y;
    for var i in 0..<100 do
      var cmp: (int, int) -> int := g;
      Console.WriteLine(cmp(1, 2) + g(4, 1));
    end for;
    Console.WriteLine("DONE");
    return 0;
  end Main;
end C;
)BZL"), e);
}

// CONTROL: a producer-initialized local reassigned TO the source (the pre-existing
// #4174 path) stays clean.
BEZEL_TEST(closure_local_alias_reassign_uaf_8440,
           control_producer_local_reassigned_to_source_spec_28_9) {
    std::string e;
    for (int i = 0; i < 100; ++i) { line(e, i % 2 == 0 ? 2 : 4); }
    e += "DONE\n";
    BEZEL_EXPECT_EQ(runCheckedOut(R"BZL(
module M;
public class C
  public static method Main(): int
    var g: (int, int) -> int := lambda(x: int, y: int) => x - y;
    for var i in 0..<100 do
      var cmp: (int, int) -> int := lambda(x: int, y: int) => y - x;
      if i mod 2 = 0 then
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
