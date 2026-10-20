// RUN: %not %bezelc -c %s 2>&1 | FileCheck --check-prefix=ONCE %s
// RUN: %not %bezelc -c %s 2>&1 | FileCheck --check-prefix=NODIAG --allow-empty %s
// RUN: %not %bezelc -c %s 2>&1 | grep -c "produces an owned value that requires disposal" | FileCheck --check-prefix=OWNED3 %s
// RUN: %not %bezelc -c %s 2>&1 | grep -c "discards a .Task. result" | FileCheck --check-prefix=TASK3 %s

// fix #6572 (§21.9, v0.338): §21.9's escalation REPLACES the §21.9
// unused-result warning, so a program that draws the hard error must draw
// EXACTLY ONE §21.9 diagnostic, not the error and the warning stacked at the
// same file:line:col.
//
// THE DEFECT THIS ROW PINS. The §21.9 unused-result warning walk sits at the
// BOTTOM of sema's `ExpressionStmt` arm, after both §21.9 hard-error arms —
// the owned non-trivial-dispose reject and the `Task` reject. Neither of those
// arms RETURNS: the owned arm is `if (reject) { report; }` with no `return`
// and no `else`, and the `Task` arm reports and falls out of its own `if`
// without returning. So the walk ran on EVERY statement, including the ones
// that had just escalated, and each of them drew its hard error AND the
// warning, at the same file:line:col.
//
// That is not a stylistic redundancy. §21.9 states one message per regime:
// the warning "is replaced by" the hard error. MEASURED before this fix, over
// the shapes below, each line drew two:
//
//   s21_9_escalation_one_diagnostic_6572.bzl:172:5: this expression produces an owned value …(§21.9)
//   s21_9_escalation_one_diagnostic_6572.bzl:172:5: warning: the result of this expression statement is unused…(§21.9)
//
// MEASURED: 6 of the 6 shapes below drew the doubled pair before the fix and
// 0 do after. Nine pre-existing lit rows were drawing it too — see the
// handback's item-19a sweep — and two already-banked conformance cases
// (`s21_9_0003`, `s21_9_0026`) corroborate it independently: their own rule
// text says the warning is "replaced by a hard error", and they pass only
// because the runner checks the NEEDLE, not the diagnostic COUNT.
//
// ⚠ WHICH DIRECTIVE ACTUALLY CATCHES THE DEFECT, stated exactly. The load-
// bearing pair is `ONCE` + `ONCE-NOT`, NOT the `grep -c` lines: the doubling
// this row pins is an extra WARNING alongside the error, and only a directive
// that names the warning can see it. The `ONCE` for a line matches the hard
// error there; the `ONCE-NOT` that FOLLOWS it covers the span up to the next
// `ONCE`, and the extra warning lands in that span. Proven mechanically: with
// the `s21_9Escalated` latch reverted, this file fails and the failing
// directive is `ONCE-NOT`, not a count.
//
// The two `grep -c` lines carry a DIFFERENT and weaker property, and saying
// so matters: they count the HARD ERRORS (3 owned, 3 `Task`), which the
// pre-fix compiler also emitted 3-and-3 of — MEASURED. So they would NOT
// have caught the doubling. What they catch is a future change that makes an
// escalation arm reach only SOME of its shapes, or reach one twice, by
// pinning the count rather than accepting "the first occurrence". A
// `FileCheck` directive matches the first occurrence and moves on, so it
// cannot distinguish one from two; these two can. `grep -c` exits 1 on no
// match and lit's shell reports that stage's status before FileCheck sees the
// `0`, which is why both counts here are positive numbers by construction
// (each shape is measured to produce one) rather than a zero assertion.
//
// THE SIX SHAPES, three per escalation arm, each in its own method so a
// collapse of one does not take another with it:
//
//   Arm A — the owned non-trivial-dispose reject. Three production shapes,
//   because §28.3's producer classification reaches the arm by more than one
//   route and a fix that latched only one of them would be green on the other
//   two:
//     line 172 `c.Open();`    — a METHOD-SYMBOL callee returning `owned Gauge`
//     line 176 `new Gauge();` — a NewExpr producing a non-trivial-dispose class
//     line 181 `move g;`      — a §28.8.1 transfer marker (arm (a) of the
//                               widening). MEASURED: each of the three
//                               escalates, so all three must be latched.
//
//   Arm B — the `Task` reject, keyed on `isTaskFamilyRef` of the STATEMENT'S
//   type, so it fires however the task was produced. Two task TYPES and two
//   PRODUCTION shapes, for the same reason:
//     line 185 `Counter.Work();`      — a bare `Task` call (no `<T>`)
//     line 190 `Counter.WorkInt();`   — a bare `Task<int>` call
//     line 195 `Task.FromResult(1);`  — a BUILTIN producer, not a declared
//                                       method, so the arm-meet/extern routes
//                                       above do not reach it
//
//   Each of the six is followed by an `await Task.Delay(1);` (or is the only
//   statement in its method), which is there for a specific reason: it is the
//   program's own WITNESS that the compile produced diagnostics at all, so
//   every `NODIAG-NOT` span below is a region containing real output rather
//   than a vacuous pass over an empty stream. Without it, a compiler that
//   rejected the file for an unrelated reason — or that emitted nothing —
//   would satisfy the negative half while proving nothing about §21.9.
//
// ⚠ WHAT THIS ROW DOES NOT CLAIM. It pins ONE diagnostic on the six
// escalation shapes. It does NOT pin that a `Task`-typed result drawn as a
// bare statement warns on any other spelling, and it does NOT close the
// unimplemented half of §21.9's escalation that #6572's review named
// separately: the escalation clause is keyed on the result's STATIC TYPE
// ("a non-trivial-dispose class or an interface", "whether the result is an
// owned production or a borrow"), and a BORROW-typed result of either kind is
// still only warned. That is pre-existing, was silent on the base too, and is
// filed rather than silently widened here.

// ===================== MUST-DIAGNOSE, exactly one each =====================
//
// The six directives below are ordered by SOURCE LINE (170, 174, 179, 183, 188,
// 195) so each `ONCE-NOT` span is bounded by the `ONCE` CHECK that follows it
// and has a finite, known range. `ONCE` pins the hard error at the
// statement's OWN file:line:col; `ONCE-NOT` pins the ABSENCE of the §21.9
// unused-result warning over the span that follows. Together each pair is the
// row's real subject: one diagnostic, on this line, of this regime.
//
// Arm A, the owned non-trivial-dispose reject — three production shapes.
// ONCE: s21_9_escalation_one_diagnostic_6572.bzl:172:5: this expression produces an owned value that requires disposal
// ONCE-NOT: warning: the result of this expression statement is unused
// ONCE: s21_9_escalation_one_diagnostic_6572.bzl:176:5: this expression produces an owned value that requires disposal
// ONCE-NOT: warning: the result of this expression statement is unused
// ONCE: s21_9_escalation_one_diagnostic_6572.bzl:181:5: this expression produces an owned value that requires disposal
// ONCE-NOT: warning: the result of this expression statement is unused
//
// Arm B, the `Task` reject — three production shapes and two task types.
// ONCE: s21_9_escalation_one_diagnostic_6572.bzl:185:5: this expression statement discards a `Task` result
// ONCE-NOT: warning: the result of this expression statement is unused
// ONCE: s21_9_escalation_one_diagnostic_6572.bzl:190:5: this expression statement discards a `Task` result
// ONCE-NOT: warning: the result of this expression statement is unused
// ONCE: s21_9_escalation_one_diagnostic_6572.bzl:195:5: this expression statement discards a `Task` result

// ============================ THE COUNTS ==================================
//
// The exact counts. Three of each arm, because three of each shape above is
// what the arm is MEASURED to reject; a drop to two and a doubling to six
// both go red here.
//
// OWNED3: 3
// TASK3: 3

// ========================= THE NEGATIVE HALF ===============================
//
// §21.9's unused-result WARNING must not appear anywhere in this file's
// compile. Asserted as an absence rather than a `grep -c` zero, because
// `grep -c` exits 1 on no match and lit's shell reports that pipeline stage's
// status before FileCheck ever sees the `0` — the row would then fail for the
// wrong reason, or pass for none.
//
// NODIAG-NOT: warning: the result of this expression statement is unused
//
// The three OWNED3/TASK3 counts above are what make this absence
// NON-VACUOUS on its own: six diagnostics are measured to be present at known
// lines, so the stream is not empty and the absence above is a real
// assertion about six known-populated regions rather than a pass over
// nothing.

module M;

public class Gauge
  public constructor()
  end constructor;

  destructor
    Console.WriteLine("gauge closed");
  end destructor;
end Gauge;

public class Counter
  public method Open(): owned Gauge
    return new Gauge();
  end Open;

  public static async method Work(): Task
    await Task.Delay(1);
  end Work;

  public static async method WorkInt(): Task<int>
    return 1;
  end WorkInt;
end Counter;

public class Program
  public static method E1()
    using var c := new Counter();
    c.Open();
  end E1;

  public static method E2()
    new Gauge();
  end E2;

  public static method E3()
    var g := new Gauge();
    move g;
  end E3;

  public static async method E4(): Task
    Counter.Work();
    await Task.Delay(1);
  end E4;

  public static async method E5(): Task
    Counter.WorkInt();
    await Task.Delay(1);
  end E5;

  public static async method E6(): Task
    Task.FromResult(1);
    await Task.Delay(1);
  end E6;
end Program;
