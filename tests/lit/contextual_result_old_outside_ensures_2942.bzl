// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// Spec §22.16 (v0.240), issue #2942: `result` and `old(E)` are contextual
// keywords valid only inside `ensures` clauses (§22.16 "Clause typing"
// and Appendix B.2). Outside `ensures` — in a method body, in a
// `requires` clause, or in an `invariant` member — §22.16 forbids these
// names and the spec itself notes they fall through to ordinary identifier
// resolution. Today's behavior is the generic "unresolved identifier",
// which leaks implementation noise to the user. This lane ships a §22.16-
// cited diagnostic instead, naming the keyword and pointing at the issue.
//
// FAILS-without / PASSES-with: without the fix, every `result` / `old`
// mention outside `ensures` falls through to "unresolved identifier `X`"
// with no §22.16 citation. With the fix, the contextual-keyword diagnostic
// fires — FileCheck matches the four CHECK lines below.
//
// Axis grid — every out-of-`ensures` mention of either keyword:
//   (1) `result` in a method body
//   (2) `old` in a method body
//   (3) `result` in a `requires` clause
//   (4) `old` in a `requires` clause
//
// (The `invariant` member arms are covered semantically in
// `PureChecker::checkInvariantExpr`'s `clauseMode_` walk; the contextual
// keyword error here fires too, but the invariant-member path is exercised
// in #2941's invariant test rather than this pin.)

module M;
class C
  public static method InBody(): int
    var x := result;
// CHECK: contextual_result_old_outside_ensures_2942.bzl:31:14: `result` is a contextual keyword reserved for `ensures` clauses
    return 7;
  end InBody;

  public static method InBodyOld(): int
    var x := old;
// CHECK: contextual_result_old_outside_ensures_2942.bzl:37:14: `old` is a contextual keyword reserved for `ensures` clauses
    return 7;
  end InBodyOld;

  public static method InRequires(n: int): int
    requires(result > 0)
// CHECK: contextual_result_old_outside_ensures_2942.bzl:43:14: `result` is a contextual keyword reserved for `ensures` clauses
    return n * 2;
  end InRequires;

  public static method InRequiresOld(n: int): int
    requires(old(n) > 0)
// CHECK: contextual_result_old_outside_ensures_2942.bzl:49:14: `old` is a contextual keyword reserved for `ensures` clauses
    return n * 2;
  end InRequiresOld;
end C;
