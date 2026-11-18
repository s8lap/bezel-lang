// Regression test for #7722 — a PLAIN (non-`owned`) MOVE-ONLY PARAMETER of a
// method never disposed the argument it received, so the value leaked.
//
// THE DEFECT (measured on the base, `r30-c1` = 227d95a17, by the R42
// verification lane r42-v-c2 and re-measured here).
//
// A move-only OWNING CHOICE passed to a plain (unmarked) by-value-move
// formal LEAKED the whole value:
//
//   module M;
//   class Res
//     public static _n: int := 0;
//     public constructor() end constructor;
//     destructor Res._n := Res._n + 1; end destructor;
//   end Res;
//   choice OChoice Text(owned r: Res), Empty;
//     public method Describe(): int => 7;
//   end OChoice;
//   ... (the `Bx` helper class of the issue's repro) ...
//   class Program
//     public static method Call(f: (OChoice) -> int, o: OChoice): int return 1; end Call;
//     public static method Body()
//       var bx := new Bx(); var k := Call(lambda(o: OChoice) => 1, OChoice.Text(new Res()));
//     end Body;
//     public static method Main()
//       var i := 0;
//       while i < 100 do Body(); i := i + 1; end while;
//       Console.WriteLine(Res._n);
//     end Main;
//   end Program;
//
// prints `400` on the base — only the four `Res` objects the `Bx` helper
// owns are disposed, and NONE of the 100 the argument carries is — and
// LeakSanitizer reports "Direct leak of 1600 byte(s) in 100 object(s)".
//
// THE SPEC AUTHORITY.
//
//   * §15.3 *Parameter passing* (v0.359): "a parameter declared `(x: V)` for an
//     ordinary V is by value (copy); for a move-only V, it is by value-move
//     (transfer ownership). The caller's argument expression is in moved-from
//     state after the call." ⇒ the CALLER's binding is moved-from and disposes
//     nothing; the CALLEE holds the value.
//   * §28.1 clause 1 (v0.359): "Move-only value types (§15.3): the modifier
//     is admitted-but-redundant — the type's own move semantics already
//     transfer the obligation." §15.3 says it in full: "A parameter declared
//     `(owned x: V)` is identical to plain `(x: V)` for a move-only V; the
//     modifier is admitted for documentation but is redundant." ⇒ the `owned`
//     modifier is IRRELEVANT to this defect, and a fix that only handled the
//     `owned o: OChoice` spelling would NOT fix #7722 — the issue's own repro
//     uses the PLAIN spelling.
//   * §28.3.10 (v0.359): an owned binding that reaches a normal-exit scope end
//     undischarged "is disposed implicitly at that scope end (for an `owned`
//     parameter, at the end of the method)" — and §28.3's *Exception-path
//     discharge* covers the unwind edge.
//   * §35.4 (v0.359): "Disposal is defined by the active case" — the owning
//     choice's active case's `owned` payloads are disposed in place.
//
// THE DIFFERENTIAL (the control that pins the defect on the MODIFIER rather
// than on the shape): the `owned o: OChoice` spelling of the same parameter
// does NOT leak — it prints `100` and LeakSanitizer is clean. That is the
// differential: the two spellings are identical for a move-only V (§15.3
// *Parameter passing*; §28.1 clause 1), so the difference in the verdict was
// the missing registration. (It is green on the base as well, so it is NOT a
// gtest row — it is a corpus candidate:
// `corpus-candidates/s_owned_static_block.bzl`.)
//
// THE ROWS BELOW.
//
//   * the primary row — the issue's own repro, verbatim: 500 dispositions
//     (400 from the `Bx` helper's own `Res` objects + the 100 the argument
//     carries), LeakSanitizer clean. RED on the base (400, and a 1600-byte
//     leak).
//   * the NAMED-ARGUMENT row — the same callee, but the argument is a named
//     local rather than an unbound temporary. RED on the base (0 dispositions
//     and a 1600-byte leak): the caller's source is marked moved-from at a
//     by-value-move formal, so the CALLER disposes nothing and the callee
//     must.
//   * BOUNDARY CONTROL 1 — the `?.` spelling of the same call. GREEN on the
//     base and on the fix; RED under the plausible wrong fix "register the
//     callee-side disposal but leave the `?.` path's call-site mark alone"
//     (the caller then keeps the value and disposes it as well — a
//     heap-use-after-free). A corpus case cannot express this check: it
//     needs the sanitizer's double-disposal report.
//   * BOUNDARY CONTROL 2 — a callee whose BODY throws. GREEN on the base and
//     on the fix; RED under the plausible wrong fix "register the callee-side
//     disposal but leave the caller's pre-call mark alone" (the caller's
//     source then survives the callee's throw and both dispose — the same
//     heap-use-after-free).
//
// All four rows use a STATIC, block-bodied callee whose declaring type has no
// interface and no `requires` clause: the callee-side registration of #7722
// is admitted for exactly that shape (a callee whose call site is statically
// bound and whose entry cannot raise), so the rows exercise the FIXED path
// rather than the shape the fix leaves at the base's behaviour.
//
// The count is asserted through the DESTRUCTOR COUNTER (the primary
// measurement) and through LeakSanitizer (the second): a one-shot leak row is
// a vacuous oracle, so every row is amplified 100×.
//
// per §15.3, §28.1, §28.3, §28.3.10, §35.4, #7722

#include <cstddef>
#include <string>

#include "support/aot_result_asserts.hpp"  // checkedToolchainSkipped
#include "support/aot_support.hpp"          // runAotChecked — AOT + ASan/LSan
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::runAotChecked;

namespace {

// Count non-overlapping occurrences of `needle` in `hay`.
int countOccurrences(const std::string& hay, const std::string& needle) {
    int n = 0;
    for (std::size_t p = hay.find(needle); p != std::string::npos;
         p = hay.find(needle, p + needle.size()))
        ++n;
    return n;
}

// The LAST non-empty line of `hay`, with trailing CR/space removed.
//
// The destructor counter is printed by the program's LAST statement, so the
// last line is the count. A `"\n<count>\n"` SEARCH would be wrong for the
// primary row: that program prints NOTHING but the count, so the output is
// exactly "<count>\n" with no preceding newline and the search would fail on
// a CORRECT run. (This is the same reason `runAotOut` strips the trailing
// line-ending rather than baking it into the expectation.)
std::string lastNonEmptyLine(const std::string& hay) {
    auto isBlank = [](char c) {
        return c == '\n' || c == '\r' || c == ' ' || c == '\t';
    };
    std::size_t end = hay.size();
    while (end > 0 && isBlank(hay[end - 1])) --end;
    if (end == 0) return std::string();
    const std::size_t nl = hay.rfind('\n', end - 1);
    const std::size_t start = (nl == std::string::npos) ? 0 : nl + 1;
    std::string line = hay.substr(start, end - start);
    while (!line.empty() && isBlank(line.back())) line.pop_back();
    return line;
}

// The issue's own repro program, VERBATIM (the `a20_func_value_call.bzl`
// probe of the R37 L21 lane, as the issue quotes it). The `Res` class counts
// its own disposals in a static field, so the printed number is the
// observable.
const char* kIssueRepro =
    "module M; "
    "class Res "
    "  public static _n: int := 0; "
    "  public constructor() end constructor; "
    "  destructor Res._n := Res._n + 1; end destructor; "
    "end Res; "
    "choice OChoice Text(owned r: Res), Empty; "
    "  public method Describe(): int => 7; "
    "end OChoice; "
    "choice VC A(x: int), B; end VC; "
    "choice BC P(r: Res), Q; end BC; "
    "record Pt(x: int, y: int); "
    "class Bx "
    "  public owned V: OChoice := OChoice.Empty; "
    "  public owned NV: OChoice? := null; "
    "  public A: [2 owned]Res?; "
    "  public C: VC := VC.B; "
    "  public N: int := 5; "
    "  public S: string := \"hi\"; "
    "  public P: Pt := new Pt(1, 2); "
    "  public IA: [3]int; "
    "  public Bc: BC := BC.Q; "
    "  public owned R: Res := new Res(); "
    "  public Cls: Res? := null; "
    "  public F: (int) -> int; "
    "  public constructor() "
    "    V := OChoice.Text(new Res()); NV := OChoice.Text(new Res()); "
    "    A := [new Res(), null]; "
    "    F := lambda(x: int) => x + 1; "
    "  end constructor; "
    "end Bx; "
    "class Program "
    "  public static method Call(f: (OChoice) -> int, o: OChoice): int return 1; end Call; "
    "  public static method Body() "
    "    var bx := new Bx(); var k := Call(lambda(o: OChoice) => 1, OChoice.Text(new Res())); "
    "  end Body; "
    "  public static method Main() "
    "    var i := 0; "
    "    while i < 100 do Body(); i := i + 1; end while; "
    "    Console.WriteLine(Res._n); "
    "  end Main; "
    "end Program; ";

// The counting class for the rows below: every instance bumps `Count` on
// disposal AND prints a marker, so the count is observable both in the
// captured stdout and in the printed counter.
//
// `Count` is a mutable static primitive field, which §22.12 admits with a
// declaration-site warning — the warning is irrelevant to these rows (the
// program is a single-domain test program) and does not affect the count.
const char* kRes =
    "public class Res "
    "  public static Count: int := 0; "
    "  public constructor() end constructor; "
    "  destructor Res.Count := Res.Count + 1; "
    "  Console.WriteLine(\"RES-DISPOSED\"); "
    "end destructor; "
    "end Res; ";

const char* kChoice =
    "public choice OChoice Text(owned r: Res), Empty; end OChoice; ";

}  // namespace

// =====================================================================
// §15.3 *Parameter passing* + §28.1 clause 1 + §28.3.10: the CALLEE
// owns a plain move-only parameter's value and discharges it at its
// scope end. This is the issue's own repro, amplified 100×.
// =====================================================================
//
// WITHOUT the fix: 400 dispositions (the `Bx` helper's own four `Res` per
// iteration, and NOT the argument's), and LeakSanitizer "Direct leak of
// 1600 byte(s) in 100 object(s)".
// WITH the fix: 500 dispositions, no leak report.
BEZEL_TEST(ownership_owning_choice_param_dispose_7722,
           issue_repro_plain_move_only_parameter_disposes_its_argument) {
    JitResult r = runAotChecked(std::string(kIssueRepro));
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);

    // 100 iterations × (4 `Res` the `Bx` helper owns + the 1 the argument
    // carries) = 500. On the base this prints 400 — the argument's 100 are
    // never disposed.
    BEZEL_EXPECT_EQ(lastNonEmptyLine(r.stdoutText), "500");
    // LeakSanitizer must report zero leaks with the fix in place. The harness
    // turns ANY sanitizer report — a leak, a double-free, a use-after-free —
    // into a failed run (`r.ran` false) and names it in the diagnostics, so
    // this check pins the failure MODE as well as the failure itself.
    BEZEL_EXPECT_FALSE(
        bezel::test::diagnosticMessageContains(r, "sanitizer fired"));
}

// =====================================================================
// THE NAMED-ARGUMENT SHAPE — the same callee, but the argument is a
// NAMED LOCAL rather than an unbound temporary. §15.3: "The caller's
// argument expression is in moved-from state after the call", so the
// CALLER disposes nothing here and the callee's own scope-end disposal
// is the only one.
//
// RED on the base: 0 dispositions and a 1600-byte leak (the caller's
// local is marked moved-from at the by-value-move formal, and the
// callee never disposed what it received).
// =====================================================================
BEZEL_TEST(ownership_owning_choice_param_dispose_7722,
           named_local_argument_is_disposed_exactly_once_by_the_callee) {
    std::string src =
        "module M; " + std::string(kRes) + std::string(kChoice) +
        "public class Program "
        "  public static method Take(o: OChoice): int return 1; end Take; "
        "  public static method Main() "
        "    var i := 0; "
        "    while i < 100 do "
        "      var b := OChoice.Text(new Res()); "
        "      var k := Take(b); "
        "      i := i + 1; "
        "    end while; "
        "    Console.WriteLine($\"{Res.Count}\"); "
        "  end Main; "
        "end Program;";
    JitResult r = runAotChecked(src);
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    // Exactly once — 200 would be a double-free, 0 a leak.
    BEZEL_EXPECT_EQ(countOccurrences(r.stdoutText, "RES-DISPOSED"), 100);
    BEZEL_EXPECT_EQ(lastNonEmptyLine(r.stdoutText), "100");
    BEZEL_EXPECT_FALSE(
        bezel::test::diagnosticMessageContains(r, "sanitizer fired"));
}

// =====================================================================
// BOUNDARY CONTROL 1 — the `?.` spelling of the same call. §7.5 makes
// the two spellings ONE access, so the callee owns the value on this
// path too.
//
// GREEN on the base and on the fix. RED under the plausible wrong fix
// "register the callee-side disposal but leave the `?.` path's
// call-site mark alone": the caller then keeps the value and disposes
// it as well, and the sanitizer reports a heap-use-after-free (the
// callee freed the block; the caller read it). A corpus case cannot
// express this check — it needs the sanitizer's double-disposal
// report.
// =====================================================================
BEZEL_TEST(ownership_owning_choice_param_dispose_7722,
           null_conditional_call_does_not_dispose_the_argument_twice) {
    std::string src =
        "module M; " + std::string(kRes) + std::string(kChoice) +
        "public class Holder "
        "  public method Take(o: OChoice): int return 1; end Take; "
        "end Holder; "
        "public class Program "
        "  public static method Main() "
        "    var i := 0; "
        "    while i < 100 do "
        "      var b := OChoice.Text(new Res()); "
        "      var h: Holder? := new Holder(); "
        "      var k := h?.Take(b); "
        "      i := i + 1; "
        "    end while; "
        "    Console.WriteLine($\"{Res.Count}\"); "
        "  end Main; "
        "end Program;";
    JitResult r = runAotChecked(src);
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(countOccurrences(r.stdoutText, "RES-DISPOSED"), 100);
    BEZEL_EXPECT_EQ(lastNonEmptyLine(r.stdoutText), "100");
    BEZEL_EXPECT_FALSE(
        bezel::test::diagnosticMessageContains(r, "sanitizer fired"));
}

// =====================================================================
// BOUNDARY CONTROL 2 — a callee whose BODY throws. §28.3's
// *Exception-path discharge* disposes the value on the unwind edge, and
// the caller's source is marked moved-from BEFORE the call, so the
// callee's disposal is the only one.
//
// GREEN on the base and on the fix. RED under the plausible wrong fix
// "register the callee-side disposal but leave the caller's pre-call
// mark alone": the caller's source then survives the callee's throw
// (the post-call sweep runs only on the normal-return edge) and both
// dispose — the same heap-use-after-free.
// =====================================================================
BEZEL_TEST(ownership_owning_choice_param_dispose_7722,
           throwing_callee_does_not_dispose_the_argument_twice) {
    std::string src =
        "module M; " + std::string(kRes) + std::string(kChoice) +
        "public class Program "
        "  public static method Take(o: OChoice): int "
        "    throw new InvalidOperationException(\"boom\"); "
        "  end Take; "
        "  public static method Main() "
        "    var i := 0; "
        "    while i < 100 do "
        "      var b := OChoice.Text(new Res()); "
        "      try "
        "        var k := Take(b); "
        "      catch "
        "        InvalidOperationException => () "
        "      end try; "
        "      i := i + 1; "
        "    end while; "
        "    Console.WriteLine($\"{Res.Count}\"); "
        "  end Main; "
        "end Program;";
    JitResult r = runAotChecked(src);
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(countOccurrences(r.stdoutText, "RES-DISPOSED"), 100);
    BEZEL_EXPECT_EQ(lastNonEmptyLine(r.stdoutText), "100");
    BEZEL_EXPECT_FALSE(
        bezel::test::diagnosticMessageContains(r, "sanitizer fired"));
}
