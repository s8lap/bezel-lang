// Codegen — issue #8478: a closure local bound from another function value
// (`var cmp := g;`) and CAPTURED by a lambda — never reassigned — handed its
// SOURCE's reference to the capture cell, and the capturing lambda's env
// destructor then freed a function value the source still called.
//
// THE RULE (§27.5, v0.357): a lambda that references a variable of its
// enclosing scope captures it, and the capture lives as long as the closure
// does. §28.9 (v0.357, "Runtime-managed types"): a function-typed value is
// runtime-managed — "the type's lifetime is managed by the runtime without
// user involvement" — so capturing one name of a function value cannot end
// the life of the value under its other name. (§28.9 leaves the lifecycle
// MECHANISM to the implementation; the citations are for the observable
// guarantee.)
//
// ROOT CAUSE. The same as issue #8479's (see
// closure_local_alias_source_overwrite_8479_test.cpp): `var cmp := g;` stored
// `g`'s pointer with no retain. A lambda's first capture of a closure local
// MOVES the slot's value into a heap capture cell, and the lambda's env
// destructor releases the cell's contents as a reference the cell owns. The
// alias owned none, so the release dropped `g`'s: `g` — or the next
// iteration's `cmp` — then called freed memory.
//
// THE FIX. Every borrow-initialized closure local takes its own reference at
// the declaration (`closureAliasOwned_` no longer requires the body to assign
// to the alias), so the reference the capture cell receives, and the env
// destructor releases, is the alias's.
//
// Every program loops 200 times and runs under generated-code
// AddressSanitizer AND LeakSanitizer (`runAotChecked`), with the full stdout
// compared against an expectation computed here, in C++, from the program's
// arithmetic.
//
// FAIL-WITHOUT, MEASURED on the pre-fix compiler (r30-c1 at 0cb32bb14,
// `bezelc --sanitize=address --link=system`, ASAN_OPTIONS=detect_leaks=1):
//
//   row                                                                   pre-fix                                       with the fix
//   issue_repro_local_source_alias_captured_spec_27_5                     heap-use-after-free in lam$1, 1 lines         clean, 201 lines
//   issue_repro_parameter_source_alias_captured_spec_27_5                 heap-use-after-free in main, 0 lines          leak 64 B, 201 lines
//   alias_captured_by_two_lambdas_spec_27_5                               heap-use-after-free in lam$1, 1 lines         clean, 201 lines
//   alias_captured_and_called_directly_spec_27_5                          heap-use-after-free in lam$1, 1 lines         clean, 201 lines
//   field_alias_captured_then_field_stored_over_spec_27_5                 heap-use-after-free in lam$0, 0 lines         clean, 201 lines
//   captured_alias_returned_by_name_spec_27_5                             leak 12800 B, 401 lines                       clean, 401 lines
//   captured_alias_reassigned_then_returned_spec_27_5                     leak 12800 B, 401 lines                       clean, 401 lines
//   control_alias_of_capturing_source_captured_lambda_returned_spec_27_5  clean, 401 lines                              clean, 401 lines
//   captured_alias_source_reassigned_and_called_in_loop_spec_27_5         heap-use-after-free in lam$1, 0 lines         clean, 601 lines
//
// ("N lines" is the stdout line count before the process ended. The leak on
// the parameter row predates the fix and is not the alias's: see the row.
// The `control_` row is the suite's boundary control; it says which wrong
// fix turns it red.)

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

// THE ISSUE'S FIRST REPRO, verbatim (200 iterations). `cmp` aliases `g` and is
// captured by `rd`; nothing is reassigned. rd() = -1, g(4, 1) = 3. Pre-fix:
// `rd`'s env destructor released the capture cell's contents — `g`'s reference
// — and the next iteration read the freed cell (heap-use-after-free in `lam$1`,
// 1 line).
BEZEL_TEST(closure_local_alias_captured_8478,
           issue_repro_local_source_alias_captured_spec_27_5) {
    std::string e;
    for (int i = 0; i < 200; ++i) line(e, 2);
    e += "DONE\n";
    BEZEL_EXPECT_EQ(runCheckedOut(R"BZL(
module M;
public class C
  public static method Main(): int
    var g: (int, int) -> int := lambda(x: int, y: int) => x - y;
    for var i in 0..<200 do
      var cmp: (int, int) -> int := g;
      var rd := lambda() => cmp(1, 2);
      Console.WriteLine(rd() + g(4, 1));
    end for;
    Console.WriteLine("DONE");
    return 0;
  end Main;
end C;
)BZL"), e);
}

// THE ISSUE'S PARAMETER TWIN, verbatim. Run(g) = -1 + 3, plus g(4, 1) = 3.
// Pre-fix: heap-use-after-free in `main`, 0 lines. LSan off for this row only:
// `main`'s `g`, passed as a plain argument, is reported leaked ONCE (64 B, 1
// object, allocated in `main`) with or without an alias in the callee — the
// alias-free control leaks identically before and after (tracked as #8482).
BEZEL_TEST(closure_local_alias_captured_8478,
           issue_repro_parameter_source_alias_captured_spec_27_5) {
    std::string e;
    for (int i = 0; i < 200; ++i) line(e, 5);
    e += "DONE\n";
    BEZEL_EXPECT_EQ(runLenientOut(R"BZL(
module M;
public class C
  public static method Run(f: (int, int) -> int): int
    var cmp: (int, int) -> int := f;
    var rd := lambda() => cmp(1, 2);
    return rd() + f(4, 1);
  end Run;
  public static method Main(): int
    var g: (int, int) -> int := lambda(x: int, y: int) => x - y;
    for var i in 0..<200 do
      Console.WriteLine(C.Run(g) + g(4, 1));
    end for;
    Console.WriteLine("DONE");
    return 0;
  end Main;
end C;
)BZL"), e);
}

// Two lambdas capture the same alias (the second capture is a RE-capture, which
// retains). a() = -1, b() = 3, g(4, 1) = 3.
BEZEL_TEST(closure_local_alias_captured_8478,
           alias_captured_by_two_lambdas_spec_27_5) {
    std::string e;
    for (int i = 0; i < 200; ++i) line(e, 5);
    e += "DONE\n";
    BEZEL_EXPECT_EQ(runCheckedOut(R"BZL(
module M;
public class C
  public static method Main(): int
    var g: (int, int) -> int := lambda(x: int, y: int) => x - y;
    for var i in 0..<200 do
      var cmp: (int, int) -> int := g;
      var a := lambda() => cmp(1, 2);
      var b := lambda() => cmp(5, 2);
      Console.WriteLine(a() + b() + g(4, 1));
    end for;
    Console.WriteLine("DONE");
    return 0;
  end Main;
end C;
)BZL"), e);
}

// The captured alias is also called through its own name, before and after the
// source: -1 + 8 + 3 + 1 = 11.
BEZEL_TEST(closure_local_alias_captured_8478,
           alias_captured_and_called_directly_spec_27_5) {
    std::string e;
    for (int i = 0; i < 200; ++i) line(e, 11);
    e += "DONE\n";
    BEZEL_EXPECT_EQ(runCheckedOut(R"BZL(
module M;
public class C
  public static method Main(): int
    var g: (int, int) -> int := lambda(x: int, y: int) => x - y;
    for var i in 0..<200 do
      var cmp: (int, int) -> int := g;
      var rd := lambda() => cmp(1, 2);
      Console.WriteLine(rd() + cmp(9, 1) + g(4, 1) + cmp(2, 1));
    end for;
    Console.WriteLine("DONE");
    return 0;
  end Main;
end C;
)BZL"), e);
}

// Both triggers at once: the alias of a FIELD is captured, then the field is
// stored over. The capturing lambda still calls the value the alias was bound
// to. i = 0: -1 + -3; later: 1 + -3.
BEZEL_TEST(closure_local_alias_captured_8478,
           field_alias_captured_then_field_stored_over_spec_27_5) {
    std::string e;
    for (int i = 0; i < 200; ++i) line(e, i == 0 ? -4 : -2);
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
      var cmp: (int, int) -> int := h.F;
      var rd := lambda() => cmp(1, 2);
      h.F := lambda(x: int, y: int) => y - x;
      Console.WriteLine(rd() + h.F(4, 1));
    end for;
    Console.WriteLine("DONE");
    return 0;
  end Main;
end C;
)BZL"), e);
}

// The captured alias is also RETURNED by name. A captured local that is
// returned transfers the capture cell's reference to the caller (fix #4198's
// protocol: the capturing env's destructor does not release it), so the alias
// must own the reference it hands over. Mk prints h() + k = 2 + k; the caller
// prints f(3, 1) + f(1, 3) = 0. Pre-fix: no memory error, but the return
// minted on top of a reference nothing released (64 B per call).
BEZEL_TEST(closure_local_alias_captured_8478,
           captured_alias_returned_by_name_spec_27_5) {
    std::string e;
    for (int i = 0; i < 200; ++i) { line(e, 2 + i); line(e, 0); }
    e += "DONE\n";
    BEZEL_EXPECT_EQ(runCheckedOut(R"BZL(
module M;
public class C
  public static method Mk(k: int, c: bool): (int, int) -> int
    var g: (int, int) -> int := lambda(x: int, y: int) => x - y;
    var cmp: (int, int) -> int := g;
    var h := lambda(): int => cmp(3, 1);
    Console.WriteLine(h() + k);
    return cmp;
  end Mk;
  public static method Main(): int
    for var i in 0..<200 do
      var f := C.Mk(i, i mod 2 = 0);
      Console.WriteLine(f(3, 1) + f(1, 3));
    end for;
    Console.WriteLine("DONE");
    return 0;
  end Main;
end C;
)BZL"), e);
}

// The row above with the captured alias conditionally reassigned (through
// the capture cell) before `h` is called and the alias is returned.
BEZEL_TEST(closure_local_alias_captured_8478,
           captured_alias_reassigned_then_returned_spec_27_5) {
    std::string e;
    for (int i = 0; i < 200; ++i) { line(e, (i % 2 == 0 ? -2 : 2) + i); line(e, 0); }
    e += "DONE\n";
    BEZEL_EXPECT_EQ(runCheckedOut(R"BZL(
module M;
public class C
  public static method Mk(k: int, c: bool): (int, int) -> int
    var g: (int, int) -> int := lambda(x: int, y: int) => x - y;
    var cmp: (int, int) -> int := g;
    var h := lambda(): int => cmp(3, 1);
    if c then
      cmp := lambda(x: int, y: int) => y - x;
    end if;
    Console.WriteLine(h() + k);
    return cmp;
  end Mk;
  public static method Main(): int
    for var i in 0..<200 do
      var f := C.Mk(i, i mod 2 = 0);
      Console.WriteLine(f(3, 1) + f(1, 3));
    end for;
    Console.WriteLine("DONE");
    return 0;
  end Main;
end C;
)BZL"), e);
}

// The capturing lambda is RETURNED, and the alias it captured names a closure
// that itself captures a local: the closure and its capture outlive the
// frame through the capture cell. Mk prints g(1, 1) = k; the caller prints
// h() + h() = 2 (k - 1). BOUNDARY CONTROL: clean before the fix too (the
// unowned alias kept its source from ever being released). It goes RED under
// the obvious wrong fix — giving the alias its own reference and leaving the
// SOURCE classified as "its closure dies with the scope": MEASURED on this
// change's first design, which did that, `k`'s capture cell was freed at
// Mk's end and the caller's `h()` read it (heap-use-after-free in the lambda
// body, 1 line).
BEZEL_TEST(closure_local_alias_captured_8478,
           control_alias_of_capturing_source_captured_lambda_returned_spec_27_5) {
    std::string e;
    for (int i = 0; i < 200; ++i) { line(e, i); line(e, 2 * i - 2); }
    e += "DONE\n";
    BEZEL_EXPECT_EQ(runCheckedOut(R"BZL(
module M;
public class C
  public static method Mk(k: int): () -> int
    var kk: int := k;
    var g: (int, int) -> int := lambda(x: int, y: int) => x - y + kk;
    var cmp: (int, int) -> int := g;
    var rd: () -> int := lambda() => cmp(1, 2);
    Console.WriteLine(g(1, 1));
    return rd;
  end Mk;
  public static method Main(): int
    for var i in 0..<200 do
      var h := C.Mk(i);
      Console.WriteLine(h() + h());
    end for;
    Console.WriteLine("DONE");
    return 0;
  end Main;
end C;
)BZL"), e);
}

// The three issues' shapes together: the alias is captured (#8478), its source
// is reassigned afterwards (#8479) and the reassigned source is called at a
// call site inside a loop (#8480). rd() = -1 throughout.
BEZEL_TEST(closure_local_alias_captured_8478,
           captured_alias_source_reassigned_and_called_in_loop_spec_27_5) {
    std::string e;
    for (int i = 0; i < 200; ++i) for (int j = 0; j < 3; ++j) line(e, i % 2 == 0 ? -4 : 2);
    e += "DONE\n";
    BEZEL_EXPECT_EQ(runCheckedOut(R"BZL(
module M;
public class C
  public static method Main(): int
    for var i in 0..<200 do
      var g: (int, int) -> int := lambda(x: int, y: int) => x - y;
      var cmp: (int, int) -> int := g;
      var rd: () -> int := lambda() => cmp(1, 2);
      if i mod 2 = 0 then
        g := lambda(x: int, y: int) => y - x;
      end if;
      for var j in 0..<3 do
        Console.WriteLine(rd() + g(4, 1));
      end for;
    end for;
    Console.WriteLine("DONE");
    return 0;
  end Main;
end C;
)BZL"), e);
}
