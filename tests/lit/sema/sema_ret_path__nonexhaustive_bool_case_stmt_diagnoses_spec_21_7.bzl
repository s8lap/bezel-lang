// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// #2133 (§21.7 / §4.1 / §19.6): SOUNDNESS PIN — the bool totality grant keys on
// EXHAUSTIVENESS. Here only `true` is covered; a `false` value matches no arm
// and — in STATEMENT position — falls through with no action (§19.6, NOT
// `MatchFailureException`), reaching the method end with no return. §21.7 must
// still fire. This pins that the #2133 fix did not blanket every bool
// case-statement: only a PROVABLY exhaustive one (both `true` and `false`
// covered) is total. Sibling of sema_ret_path__nonexhaustive_choice_case_stmt.
//
// Cite spec §21.7, §4.1, §19.6 (v0.226).

module M;
class C
  public method Classify(b: bool): int
    case b of
      true => return 1;
    end;
  end Classify;
end C;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: method `Classify` has a return type but does not return a value on every path
// CHECK-SAME: (§21.7)
