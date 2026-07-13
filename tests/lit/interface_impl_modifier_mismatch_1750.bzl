// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// (`%not` = LLVM's exit-status inverter; the compile is expected to fail —
// see tests/lit/README.md for why every reject test wraps `bezelc` in `not`.)
//
// Issue #1750 / spec §11.2 (v0.184, "A member matches an interface member
// when it ... has the same parameter modifiers (`ref`, `out`, `owned`,
// `scoped` — v0.184) ..."): an implementing class's method must match the
// interface member's parameter modifiers EXACTLY. §11.2's "`owned` in
// interface signatures" subsection names this specific shape as a soundness
// hazard: "Tightening from `T` to `owned T` in a parameter would create
// undischarged ownership ... the implementation would attempt to take
// ownership of a value the caller intended to retain, leaving the value's
// original owner with a dangling reference" — and the reverse, dropping
// `owned` (as here), is rejected by the same exact-match rule.
//
// THE BUG THIS PINS (#1750): `Resolver::checkAbstractImplementations()`
// (src/sema/TypeResolution_Checks.cpp) verified only member name and
// parameter COUNT for instance interface conformance; #1685 added a
// `scoped`-only comparison (`ip->modifier == "scoped"` on each side) but an
// `owned`/`ref`/`out` mismatch that did not involve `scoped` fell through
// that comparison as `false == false` and was silently accepted. Verified
// pre-fix against this exact repro (from the issue): compiled clean, exit 0.
module M;
class T end T;
interface IFactory
  method Make(owned x: T);
end IFactory;
class Impl : IFactory
  public method Make(x: T)  // drops `owned` — rejected per §11.2
  end Make;
end Impl;

// bezelc emits diagnostics as `file:line:col: message` with no "error:"
// severity word, so anchor on the `:line:col:` shape and the rule text (see
// tests/lit/ref_out_owned_local_1682.bzl for the pattern).
// CHECK: :{{[0-9]+}}:{{[0-9]+}}: method `Make` does not match interface member `Make`'s parameter modifier on parameter 1
// CHECK-SAME: `ref`/`out`/`owned`/`scoped` must match exactly
// CHECK-SAME: (§11.2
