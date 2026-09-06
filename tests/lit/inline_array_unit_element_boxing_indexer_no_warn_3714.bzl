// fix #3714 (§3.2 / §8.6 / §15.1, v0.294): negative diagnostic control for
// both #3714 halves. Before the fix, this exact program drew TWO internal
// Warning-severity diagnostics that the companion gtest
// (tests/codegen/inline_array_unit_element_boxing_indexer_3714_test.cpp)
// cannot observe (runAot/runAotChecked discard captured stderr on a
// SUCCESSFUL compile — LANE_RULES §3) — this is where they are pinned.
//
// MEASURED pre-fix (`bezelc -c`, this program's shape — the issue's own
// standalone y8.bzl/y11.bzl repros, byte-identical statements):
//   internal: the argument of this `Console.WriteLine` failed to lower to a
//     value, so the whole statement emits nothing — not even the line
//     terminator (stdlib §2.1/§21.9) — #3079           [half (a): boxing]
//   internal: indexer-setter RHS expression failed to lower (degraded —
//     target retains its previous value) — #3025       [half (b): indexer-set]
// Both are Warning-severity (compile still succeeds, exit 0 either way),
// which is exactly why this lit/FileCheck oracle — not exit code, not
// runAot's discarded-on-success diagnostics — is the one that can actually
// fail before the fix and pass after it. §15.1 (and #3528's own reading of
// it) admits `[3]()` outright, so neither diagnostic should ever fire here.
//
// RUN: %bezelc -c -o %t.o %s
// RUN: %bezelc -c %s 2>&1 | FileCheck --check-prefix=NODIAG --allow-empty %s

module N3714Lit;
class C
  public static method Main(): int
    var a: [3]() := [(), (), ()];
    var o: Object := a;
    Console.WriteLine(o.ToString());
    a[1] := ();
    Console.WriteLine(a.Length);
    return 0;
  end Main;
end C;

// NODIAG-NOT: error
// NODIAG-NOT: warning
// NODIAG-NOT: internal:
// NODIAG-NOT: #3079
// NODIAG-NOT: #3025
