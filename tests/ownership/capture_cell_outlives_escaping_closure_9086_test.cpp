// Regression test for issue #9086 — a closure that CAPTURES a local of its
// enclosing body and is KEPT past that body's scope had its capture CELL
// freed with the scope, so the still-live closure read freed memory.
//
// THE DEFECT (MEASURED on the base `r30-c1` @ 74b77d29d, `bezelc
// --sanitize=address`, the issue's own repro):
//
//   ==ERROR: AddressSanitizer: heap-use-after-free on address 0x…
//   READ of size 4 at 0x… thread T0
//       #0 0x… in lam$1 bezel
//       #1 0x… in main
//   0x… is located 0 bytes inside of 4-byte region [0x…,0x…)
//   freed by thread T0 here:
//       #0 0x… in free
//       #1 0x… in bz$M.C$Fill
//   previously allocated by thread T0 here:
//       #0 0x… in calloc
//       #1 0x… in bz$M.C$Fill
//
// The 4-byte region is the heap capture cell of the method's `int` PARAMETER
// `k` (§27.5: "Captured variables are heap-allocated in a closure object").
// It was allocated in `Fill`, freed by `Fill`'s SCOPE-END disposal, and read
// afterwards by `lam$1` — the closure that had captured it — from `main`,
// where the caller still holds that closure.
//
// WHICH TEARDOWN FREES THE CELL. The capture CELL is not owned by the
// closure's env: on the base the closure's env destructor
// (`bz$lam$1$envdtor`) was a NO-OP — it loaded the cell pointer and returned
// — and the cell was freed by the ENCLOSING FRAME's scope-end `boxedCellFree`
// (`call void @free(ptr %cell.ptr)`). The closure's env held a raw pointer to
// a cell the frame owned, so the closure could outlive the cell.
//
// The compiler has a second, refcounted route for exactly this case
// (#4509): the env announces itself with `bz_cell_retain` at the
// capture-store, drops that reference with `bz_cell_release` in its envdtor,
// and the declaring frame drops its own reference with `bz_cell_release` at
// its scope end — "the last one out frees the cell". The base took the
// PLAIN route for this capture, which is sound only when the capturing
// closure provably cannot outlive the declaring scope. It could.
//
// WHY THE ESCAPE VERDICT WAS WRONG. The escape analysis
// (`ClosureEscapeWalker` / `collectClosureReleaseEligible`) classified the
// capturing closure local `b` as NON-escaping, and that verdict is what
// selects the plain route. The classification went wrong through the #4473
// arm — the "the target is READ into a closure-typed local" arm:
//
//   if (isClosureTypedLocalAssign(*as->target) &&
//       containsTargetAnywhere(as->value.get()) && …)
//       escapedDueToAssignIntoClosureLocal_ = true;
//
// `containsTargetAnywhere` answers "NOT PROVEN ABSENT" (= true) for a node
// kind it does not enumerate, and a LAMBDA LITERAL was one of those kinds —
// the predicate cannot see inside a lambda's body. So the assignment
// `c := lambda(x: int, y: int) => 0;` inside the repro's INNER lambda body
// looked like a READ of `b`, the flag fired, and the post-walk override
// CLEARED `b`'s `escaped` verdict — discarding the only record that `b`'s
// value ALSO left the frame through a collection store (`fs.Add(b)`).
// `b` was then classified non-escaping, its capture cell was freed with the
// scope, and the kept closure read it (the UAF above).
//
// A lambda literal is a FRESH closure — a producer, not a borrow: it carries
// no part of `b`'s value into `c`, so it does not read `b` there. With the
// arm's question answered precisely for a lambda literal, the flag does not
// fire, `b` keeps its escape verdict, the capture takes the refcounted
// route, and the cell outlives every closure that captures it.
//
// THE SPEC.
//   §27.5 (v0.359) "Closures": "A lambda may reference variables from its
//   enclosing scope. Captured variables are heap-allocated in a closure
//   object, a `ref` or `out` parameter excepted."
//   §28.14 (v0.359), arm 1: "a captured local or parameter of a value type
//   that holds no borrows … lives in its heap capture cell, which the
//   closure keeps alive".
//   §28.9 (v0.359) "Runtime-managed types": "Runtime management governs the
//   *function value itself* — the heap-allocated closure object that backs it
//   — and relaxes nothing about the values the closure captures".
//
// ⇒ The capture cell is kept alive BY THE CLOSURE; it must outlive every
// closure that captures it. A scope-end free of the cell while such a
// closure is still live is a use-after-free.
//
// ORACLE. `runAotChecked` / `runAotCheckedLenient` — the checked AOT path
// with the AddressSanitizer pass over the GENERATED module. Every row LOOPS
// 200 times INSIDE the program, so the freed cell is read (or missed) many
// times over, and each row asserts the program's own output and exit code
// as well as the sanitizer's verdict.
//
// ROW 1 uses a leak-free vehicle (the escaping closure is kept in a FIELD of
// a class instance the caller DISPOSES), so it can assert the leak count as
// well. ROW 2 and ROW 3 use the issue's LITERAL repros, which keep the
// escaping closure in an `Array<Fn>`; an `Array<Fn>` does not release its
// function-typed elements when the array is disposed — a PRE-EXISTING,
// UNRELATED leak (measured independently: a program that puts a
// capture-free closure into an `Array<Fn>` and disposes the array leaks
// 12736 bytes in 199 allocations on the base too). That leak is not what
// this issue is about and is not this fix's to carry, so those two rows use
// the lenient oracle: the AddressSanitizer leg — the use-after-free this
// issue IS about — is active there as well, only the leak count is not
// asserted.
//
// MEASURED WITH THE FIX (the numbers this file asserts):
//   ROW 1  stdout "19700"  exit 0  ASan: clean  LSan: 0 leaked
//   ROW 2  stdout "81200"  exit 0  ASan: clean  LSan: 45600 B / 1800 allocs
//   ROW 3  stdout "60500"  exit 0  ASan: clean  LSan: 30400 B / 1200 allocs
//   (the two LSan figures are the pre-existing `Array<Fn>` / `Dictionary`
//   element leak described above, unchanged by this fix)

#include <iostream>
#include <string>

#include "support/aot_result_asserts.hpp"
#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::runAotChecked;
using bezel::test::runAotCheckedLenient;

namespace {

// Every diagnostic the harness produced, concatenated — the sanitizer report
// the AOT harness embeds in the diagnostic message when ASan / LSan fires.
std::string diagnosticText(const JitResult& r) {
    std::string out;
    for (const auto& d : r.diagnostics.diagnostics()) {
        out += d.message;
        out += '\n';
    }
    return out;
}

// The generated-code AddressSanitizer did NOT fire: no use-after-free, no
// out-of-bounds read. This is the defect this issue is about.
void assertNoAddressSanitizerError(const JitResult& r) {
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    const std::string text = diagnosticText(r);
    if (text.find("ERROR: AddressSanitizer") != std::string::npos)
        std::cout << "    [ ASAN REPORT ] " << text << "\n";
    BEZEL_EXPECT_TRUE(text.find("ERROR: AddressSanitizer") == std::string::npos);
}

// LeakSanitizer counted nothing: 0 leaked allocations, 0 leaked bytes. LSan
// prints a SUMMARY line only when it finds a leak, so "no summary" is 0.
void assertNoLeak(const JitResult& r) {
    const std::string text = diagnosticText(r);
    unsigned long long leakedBytes = 0, leakedAllocs = 0;
    const bool sawSummary =
        bezel::test::aot_detail::parseLeakSummary(text, leakedBytes, leakedAllocs);
    if (sawSummary)
        std::cout << "    [ LEAK COUNT ] " << leakedAllocs
                  << " allocation(s) / " << leakedBytes
                  << " byte(s) leaked (expected 0 / 0)\n";
    BEZEL_EXPECT_TRUE(sawSummary == false);
    BEZEL_EXPECT_TRUE(text.find("ERROR: LeakSanitizer") == std::string::npos);
}

// ROW 1 — the issue's shape, with the escaping closure kept in a FIELD of a
// class instance the caller DISPOSES, so the program is leak-clean and the
// row can assert the leak count as well.
//
//   Fill(box, flag, k)
//     var g: Fn := lambda(x: int, y: int) => 0;
//     var b: Fn := lambda(x: int, y: int) => x - y + k;   // captures `k`
//     var run := lambda(z: int): int => do                // the SYNC lambda
//       var c: Fn := lambda(x: int, y: int) => x + z;     // with its OWN
//       if z mod 2 = 0 then box.F := c; end;              // kept closure local
//       c := lambda(x: int, y: int) => 0;                 // (the inner keep)
//       return 0;
//     end;
//     _ := run(k);
//     if flag then box.F := b; end;                       // the OUTER keep:
//     b := g;                                              // `b` escapes
//
// The caller then invokes the kept closure AFTER `Fill` returned — the read
// of the freed capture cell.
//
// RED on the base: "heap-use-after-free … READ of size 4 … in lam$1", the
// 4-byte capture cell of `k` freed by `bz$M.C$Fill`'s scope end.
BEZEL_TEST(capture_cell_outlives_escaping_closure_9086,
           kept_closure_reads_its_capture_cell_after_the_scope_end) {
    const std::string src =
        "module M;\n"
        "type Fn := (int, int) -> int;\n"
        "public class Box\n"
        "  public F: Fn := lambda(x: int, y: int) => 0;\n"
        "end Box;\n"
        "public class C\n"
        "  public static method Fill(box: Box, flag: bool, k: int)\n"
        "    var g: Fn := lambda(x: int, y: int) => 0;\n"
        "    var b: Fn := lambda(x: int, y: int) => x - y + k;\n"
        "    var run := lambda(z: int): int => do\n"
        "      var c: Fn := lambda(x: int, y: int) => x + z;\n"
        "      if z mod 2 = 0 then box.F := c; end;\n"
        "      c := lambda(x: int, y: int) => 0;\n"
        "      return 0;\n"
        "    end;\n"
        "    _ := run(k);\n"
        "    if flag then box.F := b; end;\n"
        "    b := g;\n"
        "  end Fill;\n"
        "  public static method Main(): int\n"
        "    var total := 0;\n"
        "    for var i in 0..<200 do\n"
        "      using var box: Box := new Box();\n"
        "      Fill(box, true, i);\n"
        "      total := total + box.F(1, 2);\n"
        "    end for;\n"
        "    Console.WriteLine(total);\n"
        "    return 0;\n"
        "  end Main;\n"
        "end C;\n";
    const JitResult r = runAotChecked(src);
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, "19700\n");
    assertNoAddressSanitizerError(r);
    assertNoLeak(r);
}

// ROW 2 — the ISSUE'S FIRST LITERAL REPRO, verbatim: the escaping closure is
// kept in an `Array<Fn>` the caller holds and invokes after `Fill` returned,
// and the inner sync lambda keeps its own closure local `c` in the same
// array. `Fill` is called twice-per-`k` through `run(k)` and `run(k + 1)`.
//
// The `Array<Fn>` does not release its function-typed elements, so this
// program leaks (a pre-existing, unrelated defect — see the header); the
// lenient oracle keeps the AddressSanitizer leg, which is the leg this issue
// is about.
//
// RED on the base: the same "heap-use-after-free … READ of size 4 … in
// lam$1", freed by `bz$M.C$Fill`'s scope end.
BEZEL_TEST(capture_cell_outlives_escaping_closure_9086,
           issue_repro_kept_closure_in_an_array_reads_the_freed_cell) {
    const std::string src =
        "module M;\n"
        "type Fn := (int, int) -> int;\n"
        "public class H\n"
        "  public F: Fn := lambda(x: int, y: int) => 0;\n"
        "end H;\n"
        "public class C\n"
        "  public static method Boom(k: int)\n"
        "    if k mod 2 = 1 then\n"
        "      throw new InvalidOperationException(\"x\");\n"
        "    end;\n"
        "  end Boom;\n"
        "  public static method Fill(fs: Array<Fn>, d: Dictionary<int, Fn>, h: H, flag: bool, k: int)\n"
        "    var g: Fn := lambda(x: int, y: int) => 0;\n"
        "    var b: Fn := lambda(x: int, y: int) => x - y + k;\n"
        "    var run := lambda(z: int): int => do\n"
        "      var c: Fn := lambda(x: int, y: int) => x + z;\n"
        "      if z mod 2 = 0 then fs.Add(c); end;\n"
        "      c := lambda(x: int, y: int) => 0;\n"
        "      return 0;\n"
        "    end;\n"
        "    _ := run(k);\n"
        "    _ := run(k + 1);\n"
        "    if flag then fs.Add(b); end;\n"
        "    b := g;\n"
        "  end Fill;\n"
        "  public static method Main(): int\n"
        "    var total := 0;\n"
        "    for var i in 0..<200 do\n"
        "      var fs: Array<Fn> := new Array<Fn>();\n"
        "      var d: Dictionary<int, Fn> := new Dictionary<int, Fn>();\n"
        "      var h: H := new H();\n"
        "      try Fill(fs, d, h, true, i); catch var e: Exception => do var z := 0; end end try;\n"
        "      for var f in fs do\n"
        "        total := total + f(1, 2) + f(4, 1);\n"
        "      end for;\n"
        "      for var e in d do\n"
        "        total := total + e.Value(1, 2) + e.Value(4, 1);\n"
        "      end for;\n"
        "      total := total + h.F(1, 2);\n"
        "    end for;\n"
        "    Console.WriteLine(total);\n"
        "    return 0;\n"
        "  end Main;\n"
        "end C;\n";
    const JitResult r = runAotCheckedLenient(src);
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, "81200\n");
    assertNoAddressSanitizerError(r);
}

// ROW 3 — the ISSUE'S SECOND LITERAL REPRO, verbatim: the same shape, but
// `Fill` is an ASYNC method that suspends (`await Task.Delay(0)`) before the
// inner lambda is created, so the capture cell is live across a suspension
// point as well.
//
// RED on the base: "heap-use-after-free … READ of size 4 … in lam$1" on
// thread T1 (the worker's thread), freed by `bz$M.C$Fill`'s scope end.
BEZEL_TEST(capture_cell_outlives_escaping_closure_9086,
           issue_repro_async_fill_kept_closure_reads_the_freed_cell) {
    const std::string src =
        "module M;\n"
        "type Fn := (int, int) -> int;\n"
        "public class C\n"
        "  public static async method Fill(fs: Array<Fn>, flag: bool, k: int): Task\n"
        "    var g: Fn := lambda(x: int, y: int) => 0;\n"
        "    var b: Fn := lambda(x: int, y: int) => x - y + k;\n"
        "    await Task.Delay(0);\n"
        "    var run := lambda(z: int): int => do\n"
        "      var c: Fn := lambda(x: int, y: int) => x + z;\n"
        "      if z mod 2 = 0 then fs.Add(c); end;\n"
        "      c := lambda(x: int, y: int) => 0;\n"
        "      return 0;\n"
        "    end;\n"
        "    _ := run(k);\n"
        "    if flag then fs.Add(b); end;\n"
        "    b := g;\n"
        "  end Fill;\n"
        "  public static async method Main(): Task<int>\n"
        "    var total := 0;\n"
        "    for var i in 0..<200 do\n"
        "      var fs: Array<Fn> := new Array<Fn>();\n"
        "      await Fill(fs, true, i);\n"
        "      for var f in fs do\n"
        "        total := total + f(1, 2) + f(4, 1);\n"
        "      end for;\n"
        "    end for;\n"
        "    Console.WriteLine(total);\n"
        "    return 0;\n"
        "  end Main;\n"
        "end C;\n";
    const JitResult r = runAotCheckedLenient(src);
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, "60500\n");
    assertNoAddressSanitizerError(r);
}

}  // namespace
