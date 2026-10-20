// fix #6555 (§21.9, v0.338): the §21.9 UNUSED-RESULT WARNING was never
// implemented, so both of the shapes §21.9 names by example in adjacent
// sentences — a discarded method-call result and a "bare accessor read" —
// compiled with NO diagnostic of any severity and exit 0.
//
// ⚠ WHY THIS IS A lit ROW AND NOT A gtest. `runAot` fills
// `JitResult::diagnostics` only on a REJECT: the child's stderr is discarded
// on the success path (tests/support/aot_support.hpp). Every program in this
// file is LEGAL and compiles, so a `runAot`-based "the warning is present"
// assertion cannot see it — the oracle would be vacuously green on the base
// and green with the fix, proving nothing. `bezelc -c` writes every
// diagnostic of any severity to stderr on an accept, which is what the first
// RUN line reads. That is also why the first RUN line is load-bearing on its
// own: it pins exit 0, so this file cannot "pass" by the program being
// rejected — the WARNING is on stderr AND the compile succeeds.
//
// THE ORACLE CANNOT PASS VACUOUSLY, and the argument is mechanical rather
// than asserted:
//
//   * The MUST-WARN side is three rows — a discarded method-call result, a
//     discarded property read, a discarded indexer read — pinning
//     `warning: the result of this expression statement is unused` at the
//     statement's OWN `file:line:col`. On the pre-fix base this program
//     compiles and prints NOTHING, so FileCheck matches none of them and the
//     row is RED. Proven mechanically in the handback (both legs, differing
//     binary digests).
//   * The MUST-NOT-WARN side is four rows — the fluent-receiver discard, the
//     `()`-returning call in both spellings, `await` on a resultless `Task`,
//     and the `_ :=` suppression sink — and each is a shape the SAME needle
//     would match if the predicate were over-broad. §21.9's exemption is not
//     decoration: without it the warning fires on essentially every stdlib
//     call site (`sb.Append(x);` is the spec's own example), so an
//     over-broad predicate is not a hypothetical failure here but the default
//     one.
//   * ⚠ The CHECK-NOT rows are NOT self-concealing, and this is the part that
//     makes the negative half worth anything. A `CHECK-NOT` passes vacuously
//     if its span contains no diagnostic at all — which is also exactly what a
//     compiler that never warns would produce everywhere. So every MUST-NOT
//     span below is BOUNDED BY A MUST-WARN `CHECK` that follows it in the
//     directive order: `QuietFluent`'s span is closed by the `sb.ToString();`
//     CHECK one line below its two `Append` CHECK-NOTs (a discarded call on
//     the same receiver that MUST warn, so the span provably contains a
//     diagnostic), and `QuietUnit`'s span is closed by the `c.value;` /
//     `c.Bump(1);` CHECKs inside the same method. A compiler that warned
//     nowhere would fail the MUST-WARN rows; a compiler that warned
//     everywhere would fail these. Only a compiler that warns on the right
//     LINES passes, and the line:col in every needle is what says "right".
//
// ⚠ THE COMPILER FIX IS IN SEMA'S `ExpressionStmt` ARM, so the escalation this
// warning is "replaced by" for a non-trivial-dispose / interface / `Task`
// result is reachable independently and was NOT changed here. MEASURED on this
// base, both hard-error arms already fire: `Open();` over `Open(): owned
// Gauge` (a class with a destructor) and a bare `Task.FromResult(1);` each
// reject with a §21.9 error. The brief's premise that they were unreachable
// is REFUTED — they key on their own predicates, not on this warning. What
// the base rule being absent did leave unreachable is §21.9's EXEMPTION,
// which had nothing to exempt from. See the handback for the full argument.
//
// Note on `CHECK-NOT` and the `#6555` tail: the needle below is the PREFIX
// `this expression statement discards a non-`()` result`, deliberately not
// the full sentence and not the issue number, so that a future rewording of
// the remedy list does not silently un-pin every row here.
//
// RUN: %bezelc -c -o %t.o %s
// RUN: %bezelc -c %s 2>&1 | FileCheck %s

module M;

public class Gauge
  public constructor()
  end constructor;

  destructor
    Console.WriteLine("gauge closed");
  end destructor;
end Gauge;

public class Counter
  public value: int;

  public constructor(start: int)
    this.value := start;
  end constructor;

  public method Bump(by: int): int
    this.value += by;
    return this.value;
  end Bump;

  public method Reset(): ()
    this.value := 0;
  end Reset;

  // The OTHER spelling of a procedure (§22.1: "A method has a return type
  // after `:`, or no `:` clause for a procedure (return type `()`)") — pinned
  // beside `Reset(): ()` so a fix that keyed on the SPELLING of the return type
  // rather than the resolved `()` admits one and rejects the other.
  public method Noop()
  end Noop;

  public method Open(): owned Gauge
    return new Gauge();
  end Open;

  public async method Wait(): Task
    await Task.Delay(1);
  end Wait;
end Counter;

public class Program
  // §21.9's MUST-WARN leg 1: a discarded METHOD-CALL result. This is the
  // exact shape §21.9's own first sentence names and the shape the filed
  // issue turned on: `d[k] = v` written where `=` is §8.4 equality, so the
  // comparison is evaluated and thrown away.
  public static method WarnCall()
    var c := new Counter(0);
    c.Bump(1);
  end WarnCall;

  // §21.9's MUST-WARN leg 2: a bare ACCESSOR READ. §21.9's own first
  // paragraph admits this shape explicitly — "a **bare accessor read**
  // (`obj.SomeProperty;`, `obj[i];`) *is* an admissible expression statement:
  // the getter runs … and its result is discarded under the next paragraph's
  // rules, exactly as a call's result is".
  public static method WarnAccessor()
    var c := new Counter(0);
    c.value;
  end WarnAccessor;

  // §21.9's MUST-WARN leg 3: a bare INDEXER read — the second spelling the
  // same paragraph names, kept as its own row so a fix that handles the
  // property read but not the indexer read is caught here.
  public static method WarnIndexer()
    var xs: Array<int> := [10, 20, 30];
    xs[1];
  end WarnIndexer;

  // §21.9's MUST-NOT-WARN leg 1: the fluent-receiver EXEMPTION.
  //
  // §21.9: "A discarded method-call result draws no warning when it is a
  // §28.3 borrow whose source set lies within the statement's receiver chain
  // AND whose static type equals the receiver's static type — the `return
  // this` fluent shape (`sb.Append(x);`, stdlib §3.1, chained calls
  // included): the caller still holds the receiver, so nothing computed is
  // lost."
  //
  // Both stated conditions are load-bearing and both are pinned here by the
  // shape: `Append` returns `StringBuilder` (§3.1: "Methods return `this` to
  // allow chaining"), which is a borrow of the receiver and equals the
  // receiver's static type. The chained spelling is included because §21.9
  // says "chained calls included", and it reaches the exemption through the
  // OUTER call's receiver (the inner call), so it is a distinct walk.
  //
  // ⚠ The `sb.ToString();` line below is INSIDE this method on purpose and is
  // the row's own non-vacuity witness: it is a discarded call on the SAME
  // receiver returning the SAME `string` static type `ToString` always
  // returns, and it MUST warn (the exemption's type test is `StringBuilder ==
  // StringBuilder`, which `string` fails). So this span is not a region of
  // silence — it is a warned statement sitting one line below an un-warned
  // one, which is exactly the discrimination the rule is about.
  public static method QuietFluent()
    using var sb := new StringBuilder();
    sb.Append("a");
    sb.Append("b").Append("c");
    sb.ToString();
  end QuietFluent;

  // §21.9's MUST-NOT-WARN leg 2: the `()`-returning call.
  // §21.9: "Method calls returning `()` … are the common cases; no warning is
  // emitted for these." `Reset` declares `: ()` explicitly; `Noop` declares
  // no `:` clause at all — the OTHER spelling of a procedure (§22.1: "A
  // method has a return type after `:`, or no `:` clause for a procedure
  // (return type `()`)"). Both are pinned because a fix that keyed on the
  // SPELLING of the return type rather than the resolved `()` would admit one
  // and reject the other.
  //
  // This method also carries the two warned statements its `CHECK-NOT` needs
  // to be non-vacuous (`c.value;` and `c.Bump(1);` below).
  public static method QuietUnit()
    var c := new Counter(0);
    c.Reset();
    c.Noop();
    c.value;
    c.Bump(1);
  end QuietUnit;

  // §21.9's MUST-NOT-WARN leg 3: `await` on a resultless `Task`.
  // §21.9: "and `await` expressions on `Task` (no result) … no warning is
  // emitted for these." This is the shape whose DISCARD is the reason the
  // `Task` hard error above this walk keys on `isTaskFamilyRef`: an `await` of
  // a `Task` (no `<T>`) evaluates to `()`, while a bare `Task` STATEMENT is
  // the abandonment the hard error reports. The two must not be confused, and
  // only this row separates them.
  public static async method QuietAwait(): Task
    using var c := new Counter(0);
    await c.Wait();
  end QuietAwait;

  // §21.9's MUST-NOT-WARN leg 4: the suppression sink.
  // §21.9: "To suppress the warning, use the discard form `_ := expression;`
  // — but only when the result type allows it." `_ :=` is a declaration
  // statement (§17.13), NOT an expression statement, so §21.9's regime does
  // not reach it at all. This row pins that the sink is genuinely outside the
  // rule's site, so a fix that widened the predicate to "any discarded value"
  // rather than "any discarded EXPRESSION STATEMENT" would be caught here.
  public static method QuietDiscard()
    var c := new Counter(0);
    _ := c.Bump(1);
  end QuietDiscard;

  public static async method Main(): Task
    WarnCall();
    WarnAccessor();
    WarnIndexer();
    QuietFluent();
    QuietUnit();
    await QuietAwait();
    QuietDiscard();
    Console.WriteLine("done");
  end Main;
end Program;

// ===========================================================================
// MUST-WARN rows — one per shape §21.9 names by example.
//
// `discarded_result_warns_spec_21_9_6555.bzl` is this file's own basename. FileCheck matches a NEEDLE as a
// substring of the input, so the rows below are `<file>:<line>:<col>:` — the
// location bezelc prints — rather than the method name, which appears nowhere
// in the diagnostic. Every line:col here is the STATEMENT's own, verified by
// the handback's fail-without run, so a diagnostic that drifts onto a
// neighbouring statement fails rather than sliding.
//
// ⚠ THE ROWS ARE ORDERED BY SOURCE LINE, not grouped by leg. FileCheck walks
// its directives in order, so a CHECK-NOT only bounds the span between the
// directives around it. Every MUST-NOT-WARN span below is therefore closed by
// a MUST-WARN CHECK that comes AFTER it in this file — that ordering is what
// gives each CHECK-NOT a finite, known range, and it is why the four
// MUST-NOT-WARN rows can only be satisfied by the compiler actually knowing
// which line it is warning on.
// ===========================================================================

// Leg 1: the discarded method-call result (`c.Bump(1);`, line 105) — §21.9's
// own first sentence, and the shape the filed issue turned on: `d[k] = v`
// written where `=` is §8.4 equality, so the comparison is evaluated and
// thrown away.
// CHECK: discarded_result_warns_spec_21_9_6555.bzl:117:5: warning: the result of this expression statement is unused

// Leg 2: the bare ACCESSOR read §21.9's first paragraph admits explicitly
// (`c.value;`, line 115). §21.9's exemption "deliberately covers method calls
// only, **not accessor reads**", so a fix that keyed its exemption on the type
// test ALONE — dropping the method-call-only condition — would silence this
// row too. That is what makes it load-bearing rather than a duplicate of
// leg 1.
// CHECK: discarded_result_warns_spec_21_9_6555.bzl:127:5: warning: the result of this expression statement is unused

// Leg 3: the bare INDEXER read, the second spelling of the same paragraph
// (`xs[1];`, line 123). Kept separate from leg 2 because the indexer reaches
// the walk through a different AST node, so a fix that recognized only the
// property spelling would miss it.
// CHECK: discarded_result_warns_spec_21_9_6555.bzl:135:5: warning: the result of this expression statement is unused

// ===== MUST-NOT-WARN leg 1: the fluent-receiver EXEMPTION =====
// §21.9: "A discarded method-call result draws no warning when it is a §28.3
// borrow whose source set lies within the statement's receiver chain AND whose
// static type equals the receiver's static type — the `return this` fluent
// shape (`sb.Append(x);`, stdlib §3.1, chained calls included)."
//
// Both stated conditions are load-bearing and both are pinned by this block:
//
//   * METHOD-CALL-ONLY + TYPE EQUALITY: `sb.ToString();` (line 153) is a
//     discarded call on the SAME receiver that MUST warn — it is not fluent
//     (its result is a `string`, not the receiver's `StringBuilder`). It is
//     pinned by the CHECK immediately below these two CHECK-NOTs, which is
//     what makes those CHECK-NOTs statements about `Append` specifically
//     rather than about a method that happens to be silent. Drop either
//     condition from the exemption and this block fails: without the
//     method-call-only condition, `c.value;` (line 115) would go silent and
//     the Leg-2 CHECK above would fail; without type equality, `sb.ToString();`
//     would go silent and the CHECK below would fail.
//   * "chained calls included": line 152 reaches the exemption through the
//     OUTER call's receiver (the inner call), so it is a distinct walk from
//     line 151's and is pinned separately.
//
// CHECK-NOT: discarded_result_warns_spec_21_9_6555.bzl:163:5: warning: the result of this expression statement is unused
// CHECK-NOT: discarded_result_warns_spec_21_9_6555.bzl:164:5: warning: the result of this expression statement is unused
// CHECK: discarded_result_warns_spec_21_9_6555.bzl:165:5: warning: the result of this expression statement is unused

// ===== MUST-NOT-WARN leg 2: the `()`-returning call =====
// §21.9: "Method calls returning `()` … are the common cases; no warning is
// emitted for these."
//
// TWO SPELLINGS, because §22.1 admits a procedure either way ("A method has a
// return type after `:`, or no `:` clause for a procedure (return type `()`)"):
// `c.Reset();` (line 169, `: ()` written) and `c.Noop();` (line 170, no `:`
// clause at all). A fix that keyed on the SPELLING rather than the resolved
// `()` admits one and rejects the other, so both are pinned.
//
// The two CHECKs that FOLLOW are the two statements in the same method that
// must warn (`c.value;` line 171, `c.Bump(1);` line 172). They bound this
// span, so the CHECK-NOTs are a claim about those two specific lines and not
// about a method with no expression statements in it.
// CHECK-NOT: discarded_result_warns_spec_21_9_6555.bzl:181:5: warning: the result of this expression statement is unused
// CHECK-NOT: discarded_result_warns_spec_21_9_6555.bzl:182:5: warning: the result of this expression statement is unused
// CHECK: discarded_result_warns_spec_21_9_6555.bzl:183:5: warning: the result of this expression statement is unused
// CHECK: discarded_result_warns_spec_21_9_6555.bzl:184:5: warning: the result of this expression statement is unused

// ===== MUST-NOT-WARN leg 3: `await` on a resultless `Task` =====
// §21.9: "and `await` expressions on `Task` (no result) … no warning is
// emitted for these." This is the shape whose DISCARD is why the `Task` HARD
// ERROR keys on `isTaskFamilyRef`: awaiting a `Task` evaluates to `()`, while
// a bare `Task` STATEMENT is the abandonment that error reports. The two must
// not be confused, and only this row separates them.
//
// This is the LAST CHECK-NOT in the file, so its span runs to end of input —
// which is the whole remaining program: the `_ :=` sink at line 196 and
// `Main`'s seven statement calls at lines 212-218. So this one row also pins
// the tail of the file, and in particular that `Main`'s `WarnCall();` etc. —
// calls to `()`-returning methods — stay silent too.
// CHECK-NOT: discarded_result_warns_spec_21_9_6555.bzl:196:5: warning: the result of this expression statement is unused

// ===== MUST-NOT-WARN leg 4: the `_ :=` suppression sink =====
// §21.9: "To suppress the warning, use the discard form `_ := expression;`."
// `_ :=` is a DECLARATION statement (§17.13), not an expression statement, so
// §21.9's regime never reaches it. This row pins the rule's SITE rather than
// its severity: a fix that widened the predicate from "any discarded
// expression-statement result" to "any discarded value" would fire here.
// It is covered by the leg-3 CHECK-NOT above, whose end-of-input span
// includes line 196.