// fix #3044 (§21.7 / §19.6 / §5.4, v0.242) — THE TOTALITY FLIP. This file is
// the v0.241 test `sema_ret_path__exhaustive_enum_case_stmt_diagnoses_spec_
// 21_7_5_4.bzl` INVERTED, renamed with its verdict.
//
// Through v0.241 §21.7 recorded the opposite rule, on one stated ground: "a
// non-constant cast `E(n)` produces a raw value matching no declared constant
// (§5.4), so the no-match fall-through of an enum `case` statement is reachable
// and a trailing exit remains required". v0.242 repeals that ground with the
// rule that produced it — §5.4's cast is now checked (compile error for a
// constant, `OverflowException` at runtime) and §17.11's `default(E)` on a
// zero-less enum is an error — so "there is no value of an enum type that is
// not equal to a declared constant" (§5.4) and the ENUM constants-coverage
// bullet joins §21.7 condition (i)'s closed-value-space list.
//
// Every declared constant is covered and every arm returns, so the `case`
// counts as exiting on every path and the method needs no trailing `return`.
//
// WRONG-without: §21.7 fires "does not return a value on every path".
// CORRECT-with: no diagnostic.
//
// Clean-compile ACCEPT control: bezelc -c must exit zero and emit no diagnostics.
//
// RUN: %bezelc -c -o %t.o %s
// RUN: %bezelc -c %s 2>&1 | FileCheck --check-prefix=NODIAG --allow-empty %s

module M;
enum E Red, Green, Blue; end E;
class C
  public method Classify(e: E): int
    case e of
      Red   => return 0;,
      Green => return 1;,
      Blue  => return 2;
    end;
  end Classify;
end C;

// NODIAG-NOT: error
// NODIAG-NOT: §
