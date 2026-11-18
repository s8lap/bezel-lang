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

// =====================================================================
// THE FUNCTION-VALUE PATHS (fix-forward of the rejected first attempt,
// review r42-rev-m71). §15.3 *Parameter passing*: "for a move-only V, it
// is by value-move (transfer ownership). The caller's argument expression
// is in moved-from state after the call." §28.3: every owned value is
// disposed exactly once on every path. The callee's discharge is ONE
// decision per BODY, so it must hold for every path that reaches the body —
// including a call through a FUNCTION VALUE (§27.6 method reference, a
// function-typed field, a lambda that forwards its own parameter, a
// higher-order method). On those paths the caller keeps and disposes its
// own value, so the callee must not dispose it as well.
//
// NOT fixed here: a HIGHER-ORDER method that forwards its own parameter
// through a function value (`Call(f, o) return f(o);` with a target that
// ignores `o`) still leaks, as on r30-c1 — the forwarder cannot know whether
// the target moves the value into a local (which disposes it), so it
// declines (rule (5) of `plainMoveOnlyParamDischargedByCallee`). Discharging
// it there was the rejected attempt's use-after-free when the target DID
// move the value (review r42-s87-7722 probes d01 / d02 / d03 / d09).
// =====================================================================
namespace {
const char* kFvPrelude =
    "module M; "
    "class Res "
    "  public static N: int := 0; "
    "  public constructor() end constructor; "
    "  destructor Res.N := Res.N + 1; end destructor; "
    "end Res; "
    "choice OChoice Text(owned r: Res), Empty; end OChoice; ";
}  // namespace

// Each row below FOLDS the shapes a wrong fix breaks into one program with the
// named-local shape the fix repairs (`TakeA(a)`, a LEAK on r30-c1), so the row is
// RED on the base AND red under each wrong fix it names, while the method
// NAMES keep every shape's admission independent (the predicate is keyed by
// name). Every shape constructs one `Res` per iteration: 5 shapes x 200
// iterations = 1000 dispositions, exactly once each, no sanitizer report.
// MEASURED on r30-c1: 800 and "3200 byte(s) leaked in 200 allocation(s)" (the
// TakeA part) on all three rows.
//
// Row A — the FUNCTION-VALUE TARGETS (review r42-rev-m71 probes b01, b34,
// b35, b36): a static method reference in a local, a bound instance method
// reference, a lambda forwarding its OWN parameter to a direct call, and a
// method reference in a function-typed field. The callee's discharge is ONE
// decision per BODY, so it must hold for every path that reaches the body
// (§27.6: a method reference forms a function value over the same method).
// RED under the wrong fix "decide the callee's discharge from its shape
// alone" (70912e757: heap-use-after-free, the callee freed the payload and
// the caller's scope-end discharge read it).
BEZEL_TEST(ownership_owning_choice_param_dispose_7722,
           function_value_targets_dispose_exactly_once) {
    std::string src = std::string(kFvPrelude) +
        "class Holder "
        "  public constructor() end constructor; "
        "  public method Take2(o: OChoice): int return 1; end Take2; "
        "end Holder; "
        "class Box "
        "  public F: (OChoice) -> int; "
        "  public constructor() F := Program.Take4; end constructor; "
        "end Box; "
        "class Program "
        "  public static method TakeA(o: OChoice): int return 1; end TakeA; "
        "  public static method Take1(o: OChoice): int return 1; end Take1; "
        "  public static method Take3(o: OChoice): int return 1; end Take3; "
        "  public static method Take4(o: OChoice): int return 1; end Take4; "
        "  public static method Main() "
        "    var i := 0; "
        "    var h := new Holder(); "
        "    var bx := new Box(); "
        "    var g := lambda(o: OChoice) => Program.Take3(o); "
        "    while i < 200 do "
        "      var a := OChoice.Text(new Res()); "
        "      var ka := TakeA(a); "
        "      var f1: (OChoice) -> int := Program.Take1; "
        "      var b1 := OChoice.Text(new Res()); "
        "      var k1 := f1(b1); "
        "      var f2: (OChoice) -> int := h.Take2; "
        "      var b2 := OChoice.Text(new Res()); "
        "      var k2 := f2(b2); "
        "      var b3 := OChoice.Text(new Res()); "
        "      var k3 := g(b3); "
        "      var b4 := OChoice.Text(new Res()); "
        "      var k4 := bx.F(b4); "
        "      i := i + 1; "
        "    end while; "
        "    Console.WriteLine(Res.N); "
        "  end Main; "
        "end Program; ";
    JitResult r = runAotChecked(src);
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(lastNonEmptyLine(r.stdoutText), "1000");
    BEZEL_EXPECT_FALSE(
        bezel::test::diagnosticMessageContains(r, "sanitizer fired"));
}

// Row B — ARGUMENTS THE CALLER'S MARK CANNOT GIVE UP (review r42-rev-s76
// probes c45, c28, c28b, c14a).
//   * A property setter that forwards its implicit `value` to a plain
//     move-only formal (`h1.P := b1`). §23.3 (v0.359): "The setter's implicit
//     `value` parameter has no modifier by default and takes a borrow of the
//     property's declared type" — the assignment's source still owns the
//     value, so the forwarded-to callee must not dispose it.
//   * A getter / expression property that returns a FIELD, passed to a plain
//     move-only formal (`TakeG(h2.Get())`, `TakeP(h3.P)`): the receiver
//     still disposes the field, so the call result is not a fresh value the
//     callee owns. (Whether `return _f` is admitted at all is #6233's
//     question; the row pins only that the callee adds no second disposal.)
//   * `TakeB(b4!)` at a callee that is NOT admitted (it is formed as a
//     value): §17.8 (v0.359) classifies `x!` "by its position exactly as the
//     bare read `x` is", but only an admitted callee takes the value, so the
//     caller's source must keep it here.
// RED under the wrong fix 16ca39b96 (heap-use-after-free on the setter and
// getter shapes, and a 200-of-200 leak on the `!` shape: an argument-mark
// that peeled `!` for every call).
BEZEL_TEST(ownership_owning_choice_param_dispose_7722,
           accessor_value_field_getter_and_bang_arguments_dispose_exactly_once) {
    std::string src = std::string(kFvPrelude) +
        "class H1 "
        "  private _k: int := 0; "
        "  public constructor() end constructor; "
        "  public property P: OChoice "
        "    get => OChoice.Empty; "
        "    set => do _k := Program.TakeS(value); end; "
        "  end P; "
        "end H1; "
        "class H2 "
        "  private _f: OChoice := OChoice.Empty; "
        "  public constructor() _f := OChoice.Text(new Res()); end constructor; "
        "  public method Get(): OChoice return _f; end Get; "
        "end H2; "
        "class H3 "
        "  private _f: OChoice := OChoice.Empty; "
        "  public constructor() _f := OChoice.Text(new Res()); end constructor; "
        "  public property P: OChoice => _f; "
        "end H3; "
        "class Program "
        "  public static method TakeA(o: OChoice): int return 1; end TakeA; "
        "  public static method TakeS(o: OChoice): int return 1; end TakeS; "
        "  public static method TakeG(o: OChoice): int return 1; end TakeG; "
        "  public static method TakeP(o: OChoice): int return 1; end TakeP; "
        "  public static method TakeB(o: OChoice): int return 1; end TakeB; "
        "  public static method Main() "
        "    var fb: (OChoice) -> int := Program.TakeB; "
        "    var i := 0; "
        "    while i < 200 do "
        "      var a := OChoice.Text(new Res()); "
        "      var ka := TakeA(a); "
        "      var h1 := new H1(); "
        "      var b1 := OChoice.Text(new Res()); "
        "      h1.P := b1; "
        "      var h2 := new H2(); "
        "      var k2 := TakeG(h2.Get()); "
        "      var h3 := new H3(); "
        "      var k3 := TakeP(h3.P); "
        "      var b4: OChoice? := OChoice.Text(new Res()); "
        "      var k4 := TakeB(b4!); "
        "      i := i + 1; "
        "    end while; "
        "    Console.WriteLine(Res.N); "
        "  end Main; "
        "end Program; ";
    JitResult r = runAotChecked(src);
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(lastNonEmptyLine(r.stdoutText), "1000");
    BEZEL_EXPECT_FALSE(
        bezel::test::diagnosticMessageContains(r, "sanitizer fired"));
}

// Row C — A METHOD THAT HANDS ITS OWN PARAMETER TO A BODY ITS SCOPE END
// CANNOT SEE: through a function value whose target moves the value into a
// local (a method, `CallM`, and a lambda, `CallL`), as an indexer argument
// whose getter moves it (`FwdIx`), and through a function-typed field
// (`FwdBox`). None of those calls zero-stores the forwarder's parameter slot,
// and the target disposes the local it moved the value into, so the
// forwarder must not dispose the parameter as well (§15.3 *Parameter
// passing*, v0.359: "for a move-only V, it is by value-move (transfer
// ownership)"; §28.3: exactly once on every path).
// RED under the wrong fix 16ca39b96 (heap-use-after-free: it admitted the
// forwarder because only DIRECT-call arguments were scanned).
BEZEL_TEST(ownership_owning_choice_param_dispose_7722,
           parameter_forwarded_to_an_unseen_body_disposes_exactly_once) {
    std::string src = std::string(kFvPrelude) +
        "class Ix "
        "  public constructor() end constructor; "
        "  public property [o: OChoice]: int "
        "    get => do var x := o; return 1; end; "
        "  end; "
        "end Ix; "
        "class Box "
        "  public F: (OChoice) -> int; "
        "  public constructor() F := lambda(o: OChoice) => do var x := o; return 1; end; end constructor; "
        "end Box; "
        "class Program "
        "  public static method TakeA(o: OChoice): int return 1; end TakeA; "
        "  public static method Keep(o: OChoice): int var x := o; return 1; end Keep; "
        "  public static method CallM(f: (OChoice) -> int, o: OChoice): int return f(o); end CallM; "
        "  public static method CallL(f: (OChoice) -> int, o: OChoice): int return f(o); end CallL; "
        "  public static method FwdIx(h: Ix, o: OChoice): int return h[o]; end FwdIx; "
        "  public static method FwdBox(bx: Box, o: OChoice): int return bx.F(o); end FwdBox; "
        "  public static method Main() "
        "    var i := 0; "
        "    var h := new Ix(); "
        "    var bx := new Box(); "
        "    while i < 200 do "
        "      var a := OChoice.Text(new Res()); "
        "      var ka := TakeA(a); "
        "      var k1 := CallM(Program.Keep, OChoice.Text(new Res())); "
        "      var k2 := CallL(lambda(o: OChoice) => do var x := o; return 1; end, OChoice.Text(new Res())); "
        "      var b3 := OChoice.Text(new Res()); "
        "      var k3 := FwdIx(h, b3); "
        "      var b4 := OChoice.Text(new Res()); "
        "      var k4 := FwdBox(bx, b4); "
        "      i := i + 1; "
        "    end while; "
        "    Console.WriteLine(Res.N); "
        "  end Main; "
        "end Program; ";
    JitResult r = runAotChecked(src);
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(lastNonEmptyLine(r.stdoutText), "1000");
    BEZEL_EXPECT_FALSE(
        bezel::test::diagnosticMessageContains(r, "sanitizer fired"));
}

// Row D — AN ADMITTED METHOD FORWARDS ITS PARAMETER TO A DECLINED CONSUMER
// THAT MOVES THE VALUE INTO A LOCAL AND THEN THROWS. The declined callee is
// called with r30-c1's convention: the caller's slot is NOT zero-stored
// before the call, and the callee's local disposes the value. On the unwind
// edge nothing zero-stores the forwarder's parameter either, so a forwarder
// that discharges its own parameter disposes the value a second time (§28.3,
// v0.359: exactly once on every path; §15.3 *Parameter passing*: "for a
// move-only V, it is by value-move (transfer ownership)"). The forwarder
// must therefore decline whenever the callee does: the forward edge is
// closed in BOTH directions. Five ways the callee is declined — named as a
// function value (`Da`), virtual (`Db`), an arrow body (`De`), a class that
// implements an interface (`Df`), a contract clause (`Dg`).
// RED under the wrong fix 9fbdc5699 (heap-use-after-free: its fixed point
// propagated "declined" from the forwarder to the callee only; review
// r42-rev-s87 probes e33a / e33b / e33e / e33f / e33g).
BEZEL_TEST(ownership_owning_choice_param_dispose_7722,
           parameter_forwarded_to_a_declined_consumer_that_throws_disposes_exactly_once) {
    std::string src = std::string(kFvPrelude) +
        "class Base "
        "  public constructor() end constructor; "
        "  public virtual method Db(o: OChoice, c: bool): int var x := o; if c then throw new InvalidOperationException(\"boom\"); end; return 1; end Db; "
        "end Base; "
        "interface IX method Get(): int; end IX; "
        "class K: IX "
        "  public constructor() end constructor; "
        "  public method Get(): int return 1; end Get; "
        "  public method Df(o: OChoice, c: bool): int var x := o; if c then throw new InvalidOperationException(\"boom\"); end; return 1; end Df; "
        "end K; "
        "class Program "
        "  public static method TakeA(o: OChoice): int return 1; end TakeA; "
        "  public static method Da(o: OChoice, c: bool): int var x := o; if c then throw new InvalidOperationException(\"boom\"); end; return 1; end Da; "
        "  public static method Ma(o: OChoice, c: bool): int var r := Da(o, c); return r; end Ma; "
        "  public static method Mb(o: OChoice, c: bool): int var bb := new Base(); var r := bb.Db(o, c); return r; end Mb; "
        "  public static method KeepE(o: OChoice, c: bool): int var x := o; if c then throw new InvalidOperationException(\"boom\"); end; return 1; end KeepE; "
        "  public static method De(o: OChoice, c: bool): int => KeepE(o, c); "
        "  public static method Me(o: OChoice, c: bool): int var r := De(o, c); return r; end Me; "
        "  public static method Mf(o: OChoice, c: bool): int var kk := new K(); var r := kk.Df(o, c); return r; end Mf; "
        "  public static method Dg(o: OChoice, c: bool): int requires(true) var x := o; if c then throw new InvalidOperationException(\"boom\"); end; return 1; end Dg; "
        "  public static method Mg(o: OChoice, c: bool): int var r := Dg(o, c); return r; end Mg; "
        "  public static method Main() "
        "    var i := 0; "
        "    var unused: (OChoice, bool) -> int := Program.Da; "
        "    while i < 200 do "
        "      var a := OChoice.Text(new Res()); "
        "      var ka := TakeA(a); "
        "      try var k1 := Ma(OChoice.Text(new Res()), i mod 2 = 0); catch InvalidOperationException => () end try; "
        "      try var k2 := Mb(OChoice.Text(new Res()), i mod 2 = 0); catch InvalidOperationException => () end try; "
        "      try var k3 := Me(OChoice.Text(new Res()), i mod 2 = 0); catch InvalidOperationException => () end try; "
        "      try var k4 := Mf(OChoice.Text(new Res()), i mod 2 = 0); catch InvalidOperationException => () end try; "
        "      try var k5 := Mg(OChoice.Text(new Res()), i mod 2 = 0); catch InvalidOperationException => () end try; "
        "      i := i + 1; "
        "    end while; "
        "    Console.WriteLine(Res.N); "
        "  end Main; "
        "end Program; ";
    JitResult r = runAotChecked(src);
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(lastNonEmptyLine(r.stdoutText), "1200");
    BEZEL_EXPECT_FALSE(
        bezel::test::diagnosticMessageContains(r, "sanitizer fired"));
}

// Row E — THE SAME FORWARD THROUGH `?.`. `h?.Dn(o) ?? 0` lowers through the
// null-conditional call, which neither zero-stores the argument's slot before
// the dispatch nor sweeps it afterwards, so even the NORMAL edge leaves the
// forwarder's parameter live after the consumer disposed it. A parameter
// read as a `?.` argument is therefore not a proved hand-off, and its method
// declines (§17.8 / §15.3, v0.359; §28.3: exactly once). The two consumers
// are declined by shape: a virtual method (`Dn`) and a method of a class that
// implements an interface (`Dm`).
// RED under the wrong fix 9fbdc5699 (heap-use-after-free; review r42-rev-s87
// probes e34 / e34b).
BEZEL_TEST(ownership_owning_choice_param_dispose_7722,
           parameter_forwarded_through_a_null_conditional_call_disposes_exactly_once) {
    std::string src = std::string(kFvPrelude) +
        "interface IX method Get(): int; end IX; "
        "class H "
        "  public constructor() end constructor; "
        "  public virtual method Dn(o: OChoice): int var x := o; return 1; end Dn; "
        "end H; "
        "class H2: IX "
        "  public constructor() end constructor; "
        "  public method Get(): int return 1; end Get; "
        "  public method Dm(o: OChoice): int var x := o; return 1; end Dm; "
        "end H2; "
        "class Program "
        "  public static method TakeA(o: OChoice): int return 1; end TakeA; "
        "  public static method Mn(o: OChoice, h: H?): int var r := h?.Dn(o) ?? 0; return r; end Mn; "
        "  public static method Mm(o: OChoice, h: H2?): int var r := h?.Dm(o) ?? 0; return r; end Mm; "
        "  public static method Main() "
        "    var i := 0; "
        "    while i < 200 do "
        "      var a := OChoice.Text(new Res()); "
        "      var ka := TakeA(a); "
        "      var k1 := Mn(OChoice.Text(new Res()), new H()); "
        "      var k2 := Mm(OChoice.Text(new Res()), new H2()); "
        "      i := i + 1; "
        "    end while; "
        "    Console.WriteLine(Res.N); "
        "  end Main; "
        "end Program; ";
    JitResult r = runAotChecked(src);
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(lastNonEmptyLine(r.stdoutText), "600");
    BEZEL_EXPECT_FALSE(
        bezel::test::diagnosticMessageContains(r, "sanitizer fired"));
}

// Row F — THE PARAMETER LEAVES (OR IS REPLACED) IN A SPELLING NO MARK
// ZERO-STORES: `return move o!` (`IdB`), a §21.1 `const` alias that is
// returned (`Tc`), a case construction over `o!` followed by a throw (`Tw`),
// and an assignment TO the parameter followed by a throw (`Ts`). In each the
// value the method was handed is disposed by whoever received it, while the
// parameter slot still holds it (or holds a value another binding owns), so
// a method that discharged its parameter would dispose it twice. The scan
// admits a read of the parameter only in the exact spellings whose lowering
// is known to zero-store the slot; every other read declines the method
// (§28.3, v0.359: exactly once on every path; §15.3 *Parameter passing*).
// RED under the wrong fix 9fbdc5699 (heap-use-after-free: it looked through
// `move` / `!` / a `const` alias as if the mark did; lane r42-f35-7722 probes
// f01 / f19 / f21 / f24).
BEZEL_TEST(ownership_owning_choice_param_dispose_7722,
           parameter_moved_in_a_spelling_no_mark_zero_stores_disposes_exactly_once) {
    std::string src = std::string(kFvPrelude) +
        "choice Outer W(v: OChoice), Z; end Outer; "
        "class Program "
        "  public static method TakeA(o: OChoice): int return 1; end TakeA; "
        "  public static method IdB(o: OChoice?): OChoice return move o!; end IdB; "
        "  public static method Tc(o: OChoice): OChoice const x := o; return x; end Tc; "
        "  public static method Tw(o: OChoice?, t: bool): int var w := Outer.W(o!); if t then throw new InvalidOperationException(\"boom\"); end; return 1; end Tw; "
        "  public static method Ts(o: OChoice, t: bool): int var x := OChoice.Text(new Res()); o := x; if t then throw new InvalidOperationException(\"boom\"); end; return 1; end Ts; "
        "  public static method Main() "
        "    var i := 0; "
        "    while i < 200 do "
        "      var a := OChoice.Text(new Res()); "
        "      var ka := TakeA(a); "
        "      var b: OChoice? := OChoice.Text(new Res()); "
        "      var k1 := IdB(b); "
        "      var k2 := Tc(OChoice.Text(new Res())); "
        "      try var k3 := Tw(OChoice.Text(new Res()), i mod 2 = 0); catch InvalidOperationException => () end try; "
        "      try var k4 := Ts(OChoice.Empty, i mod 2 = 0); catch InvalidOperationException => () end try; "
        "      i := i + 1; "
        "    end while; "
        "    Console.WriteLine(Res.N); "
        "  end Main; "
        "end Program; ";
    JitResult r = runAotChecked(src);
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(lastNonEmptyLine(r.stdoutText), "1000");
    BEZEL_EXPECT_FALSE(
        bezel::test::diagnosticMessageContains(r, "sanitizer fired"));
}

// Row G — `move (if c then o else OChoice.Empty)`: a CONDITIONAL move of the
// parameter, as a local's initializer (`T3`), as the argument of an `owned`
// formal whose callee throws (`T10`) and as a returned value (`T26`). No mark
// zero-stores the slot behind the if-expression, so the method declines and
// the taken branch's value is disposed by its receiver only (§28.8.1 /
// §28.3, v0.359). The untaken branch still LEAKS the parameter, as on
// r30-c1 — that residual is NOT pinned: the row asserts the repaired
// named-local shape (`A=200`), a dispose count that never exceeds the 800
// values constructed, and no memory error. It therefore runs on `runAot`,
// whose forced-ASan leg reports a use-after-free but not a leak
// (`runAotChecked` would fail on the residual leak this row does not pin).
// RED under the wrong fix 9fbdc5699 (heap-use-after-free; lane r42-f35-7722
// probes f03 / f10 / f26).
BEZEL_TEST(ownership_owning_choice_param_dispose_7722,
           parameter_moved_through_an_if_expression_is_not_disposed_twice) {
    std::string src = std::string(kFvPrelude) +
        "class Program "
        "  public static method TakeA(o: OChoice): int return 1; end TakeA; "
        "  public static method T3(o: OChoice, c: bool): int var x := move (if c then o else OChoice.Empty); return 1; end T3; "
        "  public static method N10(owned o: OChoice, t: bool): int if t then throw new InvalidOperationException(\"boom\"); end; return 1; end N10; "
        "  public static method T10(o: OChoice, t: bool): int return N10(move (if t then o else OChoice.Empty), t); end T10; "
        "  public static method T26(o: OChoice, t: bool): OChoice return move (if t then o else OChoice.Empty); end T26; "
        "  public static method Main() "
        "    var i := 0; "
        "    while i < 200 do "
        "      var a := OChoice.Text(new Res()); "
        "      var ka := TakeA(a); "
        "      i := i + 1; "
        "    end while; "
        "    Console.WriteLine($\"A={Res.N}\"); "
        "    i := 0; "
        "    while i < 200 do "
        "      var k3 := T3(OChoice.Text(new Res()), i mod 2 = 0); "
        "      try var k10 := T10(OChoice.Text(new Res()), i mod 2 = 0); catch InvalidOperationException => () end try; "
        "      var k26 := T26(OChoice.Text(new Res()), i mod 2 = 0); "
        "      i := i + 1; "
        "    end while; "
        "    var over := 0; "
        "    if Res.N > 800 then over := 1; end if; "
        "    Console.WriteLine($\"over={over}\"); "
        "  end Main; "
        "end Program; ";
    JitResult r = bezel::test::runAot(src);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_TRUE(r.stdoutText.find("A=200") != std::string::npos);
    BEZEL_EXPECT_EQ(lastNonEmptyLine(r.stdoutText), "over=0");
    BEZEL_EXPECT_FALSE(
        bezel::test::diagnosticMessageContains(r, "sanitizer fired"));
}

// Row H — THE CALLER'S ARGUMENT IS READ TWICE IN ONE STATEMENT, OR A BORROW
// OF IT IS HELD PAST THE CALL: the same local at two formals (`Take33`), as
// an argument and inside a nested consumer call (`Take39`, and the case
// construction `Outer.W(a52, Take52(a52))`), a payload borrow taken before
// the call and read after it (`Take42`, `Take49`, and `M48` on its own
// parameter), and a callee that RETURNS a borrow of its parameter's payload
// (`PeekR`, `PeekP`); and an ARM BINDING over the method's own parameter
// that is live while the parameter is consumed inside the arm (`ArmTake`),
// stored in a borrow field (`ArmStore`) or handed to a method that stores it
// (`ArmPass`). The pre-call mark zero-stores the slot once, before the
// arguments are loaded a second time or while a borrow of the payload is
// live, so a callee that discharged the parameter would free a value the
// caller still reads. Those consumers decline (the r30-c1 behaviour: a
// bounded leak, which the row does NOT pin — hence `runAot`, whose
// forced-ASan leg reports a use-after-free but not a leak). Sema accepts
// every shape here today; if a later change rejects one (§15.3 *Parameter
// passing*, v0.359: "The caller's argument expression is in moved-from state
// after the call"), that shape leaves this row with the flip.
// RED under the wrong fix 9fbdc5699 (heap-use-after-free; lane r42-f35-7722
// probes f33 / f39 / f42 / f48 / f49 / f52 / f46b / f47b / g02 / g04 / g08).
BEZEL_TEST(ownership_owning_choice_param_dispose_7722,
           argument_read_twice_or_borrowed_past_the_call_is_not_freed_under_the_reader) {
    std::string src = std::string(
        "module M; "
        "class Res "
        "  public static N: int := 0; "
        "  public V: int := 5; "
        "  public constructor() end constructor; "
        "  destructor Res.N := Res.N + 1; end destructor; "
        "end Res; "
        "choice OChoice Text(owned r: Res), Empty; "
        "  public method Describe(): int => 7; "
        "  public method Get(): Res? "
        "    var res: Res? := null; "
        "    case this of Text(var r) => do res := r; end, Empty => () end; "
        "    return res; "
        "  end Get; "
        "end OChoice; "
        "choice Outer W(v: OChoice, k: int), Z; end Outer; "
        "class Hold "
        "  public Bor: Res? := null; "
        "  public constructor() end constructor; "
        "end Hold; "
        "class Pk "
        "  public constructor() end constructor; "
        "  public method PeekR(o: OChoice): Res? return o.Get(); end PeekR; "
        "  public method PeekP(o: OChoice): Res? "
        "    var res: Res? := null; "
        "    case o of Text(var r) => do res := r; end, Empty => () end; "
        "    return res; "
        "  end PeekP; "
        "end Pk; "
        "class Program "
        "  public static method TakeA(o: OChoice): int return 1; end TakeA; "
        "  public static method Take33(p: OChoice, q: OChoice): int return p.Describe() + q.Describe(); end Take33; "
        "  public static method Take39(p: OChoice, q: int): int return p.Describe() + q; end Take39; "
        "  public static method Inner39(p: OChoice): int return p.Describe(); end Inner39; "
        "  public static method Take42(p: OChoice): int return p.Describe(); end Take42; "
        "  public static method Take48(p: OChoice): int return p.Describe(); end Take48; "
        "  public static method M48(o: OChoice): int "
        "    var rr := o.Get(); "
        "    var n := Take48(o); "
        "    if rr <> null then n := n + rr!.V; end if; "
        "    return n; "
        "  end M48; "
        "  public static method Take49(p: OChoice): int return p.Describe(); end Take49; "
        "  public static method Take52(p: OChoice): int return p.Describe(); end Take52; "
        "  public static method TakeG(p: OChoice): int return p.Describe(); end TakeG; "
        "  public static method ArmTake(o: OChoice): int "
        "    var t := 0; "
        "    case o of Text(var r) => do t := TakeG(o); t := t + r.V; end, Empty => do end end case; "
        "    return t; "
        "  end ArmTake; "
        "  public static method ArmStore(o: OChoice, h: Hold): int "
        "    case o of Text(var r) => do h.Bor := r; end, Empty => do end end case; "
        "    return 1; "
        "  end ArmStore; "
        "  public static method Stash(h: Hold, r: Res): int h.Bor := r; return 1; end Stash; "
        "  public static method ArmPass(o: OChoice, h: Hold): int "
        "    var t := 0; "
        "    case o of Text(var r) => do t := Stash(h, r); end, Empty => do end end case; "
        "    return t; "
        "  end ArmPass; "
        "  public static method Main() "
        "    var i := 0; "
        "    var tot := 0; "
        "    while i < 200 do "
        "      var a := OChoice.Text(new Res()); "
        "      var ka := TakeA(a); "
        "      i := i + 1; "
        "    end while; "
        "    Console.WriteLine($\"A={Res.N}\"); "
        "    var pk := new Pk(); "
        "    var hd := new Hold(); "
        "    i := 0; "
        "    while i < 200 do "
        "      var a33 := OChoice.Text(new Res()); "
        "      tot := tot + Take33(a33, a33); "
        "      var a39 := OChoice.Text(new Res()); "
        "      tot := tot + Take39(a39, Inner39(a39)); "
        "      var a42 := OChoice.Text(new Res()); "
        "      var rr := a42.Get(); "
        "      var n42 := Take42(a42); "
        "      if rr <> null then tot := tot + rr!.V; end if; "
        "      var a48 := OChoice.Text(new Res()); "
        "      tot := tot + M48(a48); "
        "      var a49 := OChoice.Text(new Res()); "
        "      var keep: Res? := null; "
        "      case a49 of Text(var r) => do keep := r; end, Empty => () end; "
        "      var n49 := Take49(a49); "
        "      if keep <> null then tot := tot + keep!.V; end if; "
        "      var a52 := OChoice.Text(new Res()); "
        "      var w52 := Outer.W(a52, Take52(a52)); "
        "      var a46 := OChoice.Text(new Res()); "
        "      var r46 := pk.PeekR(a46); "
        "      if r46 <> null then tot := tot + r46!.V; end if; "
        "      var a47 := OChoice.Text(new Res()); "
        "      var r47 := pk.PeekP(a47); "
        "      if r47 <> null then tot := tot + r47!.V; end if; "
        "      var b1 := OChoice.Text(new Res()); "
        "      tot := tot + ArmTake(b1); "
        "      var b2 := OChoice.Text(new Res()); "
        "      tot := tot + ArmStore(b2, hd); "
        "      if hd.Bor <> null then tot := tot + hd.Bor!.V; end if; "
        "      var b3 := OChoice.Text(new Res()); "
        "      tot := tot + ArmPass(b3, hd); "
        "      if hd.Bor <> null then tot := tot + hd.Bor!.V; end if; "
        "      i := i + 1; "
        "    end while; "
        "    var over := 0; "
        "    if Res.N > 2400 then over := 1; end if; "
        "    Console.WriteLine($\"over={over} tot={tot}\"); "
        "  end Main; "
        "end Program; ");
    JitResult r = bezel::test::runAot(src);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_TRUE(r.stdoutText.find("A=200") != std::string::npos);
    BEZEL_EXPECT_EQ(lastNonEmptyLine(r.stdoutText), "over=0 tot=16800");
    BEZEL_EXPECT_FALSE(
        bezel::test::diagnosticMessageContains(r, "sanitizer fired"));
}

// Row I — AN ARM BINDING THAT STAYS INSIDE ITS ARM. `case o of Text(var r)`
// over the method's own parameter borrows the payload (§15.3 *Pattern
// binding*, v0.359). Where the binding is read only as the receiver of a
// scalar field (`Rd`, also in the arm's guard) or aliased by a borrowing
// `using var` (`Pin`, with an early `return` out of the arm; §28.10: such a
// `using var` "does not assume a disposal obligation"), nothing can hold the
// borrow past the arm, so the method still discharges its parameter: every
// path, the early return included, disposes the value exactly once (§28.3).
// This is the shape of #7482's corpus cases s28_10_0072 / s28_10_0073, here
// under the leak-checking oracle. RED on r30-c1 (the parameter is never
// disposed: the count stays 0).
BEZEL_TEST(ownership_owning_choice_param_dispose_7722,
           arm_binding_contained_in_its_arm_keeps_the_parameter_disposed_exactly_once) {
    std::string src = std::string(
        "module M; "
        "class Res "
        "  public static N: int := 0; "
        "  public V: int := 5; "
        "  public constructor() end constructor; "
        "  destructor Res.N := Res.N + 1; end destructor; "
        "end Res; "
        "choice OChoice Text(owned r: Res), Empty; end OChoice; "
        "class Program "
        "  public static method TakeA(o: OChoice): int return 1; end TakeA; "
        "  public static method Pin(o: OChoice, k: int): int "
        "    case o of "
        "      Text(var r) => do using var x := r; if k = 1 then return x.V; end if; end, "
        "      Empty => do end "
        "    end case; "
        "    return 0; "
        "  end Pin; "
        "  public static method Rd(o: OChoice): int "
        "    var t := 0; "
        "    case o of Text(var r) when r.V > 0 => do t := r.V; end, Text(var q) => do end, Empty => do end end case; "
        "    return t; "
        "  end Rd; "
        "  public static method Main() "
        "    var i := 0; "
        "    var tot := 0; "
        "    while i < 200 do "
        "      var a := OChoice.Text(new Res()); "
        "      var ka := TakeA(a); "
        "      var b := OChoice.Text(new Res()); "
        "      tot := tot + Pin(b, i mod 2); "
        "      var c := OChoice.Text(new Res()); "
        "      tot := tot + Rd(c); "
        "      i := i + 1; "
        "    end while; "
        "    Console.WriteLine($\"{tot} {Res.N}\"); "
        "  end Main; "
        "end Program; ");
    JitResult r = runAotChecked(src);
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(lastNonEmptyLine(r.stdoutText), "1500 600");
    BEZEL_EXPECT_FALSE(
        bezel::test::diagnosticMessageContains(r, "sanitizer fired"));
}
