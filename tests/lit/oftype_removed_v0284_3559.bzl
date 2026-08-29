// fix #3559 (stdlib §5 `System.Collections.Linq`, v0.285) — `OfType<U>()` was
// REMOVED from the documents at v0.284 (proposal 0121 R2), so the compiler
// stops accepting it.
//
// This file pins the WORDING, not merely the rejection. A member that was
// withdrawn and a member that never existed are different facts, and only the
// second is worth re-reading your source for a typo over — so the diagnostic
// has to say "REMOVED ... at v0.284", name the section that lost it, and point
// at the issue. It supersedes `oftype_unsound_relabel_not_implemented_3440.bzl`
// and `..._lazy_arm_3440.bzl`, whose interim "not yet implemented" wording was
// correct only while the owner ruling on R2 was pending.
//
// BOTH SEMA ARMS IN ONE PROGRAM. Two independent arms type `OfType` — one for a
// concrete collection receiver, one for a lazy / `Range` / `IGrouping` receiver
// — and a removal wired into only one would read as complete while half the
// operator's surface still lowered. The two calls below reach one arm each, and
// the two CHECK groups pin that they produce the SAME diagnostic. The pair is
// also the cell the issue-3440 interim ADMITTED (`U` = the sequence's own
// element type), so this file cannot pass on a pre-#3559 compiler.
//
// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s

module M3559OfType;

class C
  public static method Main(): int
    using var a := new Array<int>();
    a.Add(1);
    using var b := a.OfType<int>().ToArray();
    using var c := a.Where(lambda(x: int) => x > 0).OfType<int>().ToArray();
    return b.Count + c.Count;
  end Main;
end C;

// The collection-receiver arm. Located at the call, never 0:0.
// CHECK: :{{[1-9][0-9]*}}:{{[1-9][0-9]*}}: `OfType<int>()` on a sequence of `int`
// CHECK-SAME: was REMOVED from the standard library at v0.284 (stdlib §5)
// CHECK-SAME: the operator is gone, not misspelled (issue 3559)
// CHECK-SAME: Filter explicitly with `Where` and an `is` test (spec §12.7)

// The lazy-receiver arm reaches the same diagnostic, so the two cannot drift
// apart into two spellings of one rule.
// CHECK: :{{[1-9][0-9]*}}:{{[1-9][0-9]*}}: `OfType<int>()` on a sequence of `int`
// CHECK-SAME: was REMOVED from the standard library at v0.284 (stdlib §5)
