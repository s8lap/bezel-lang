// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// fix #3044 (§17.11 / §3.6 rule 2 / §14.2, v0.242): §17.11's closing rule — "A
// substitution whose `default` is not well-defined — an enum with no
// zero-valued constant included — is rejected at the instantiation site (the
// §3.6 rule 2 pattern)."
//
// The generic body takes `default(T)` under `where T: value`, where §17.11
// guarantees "`default(T)` is always non-null". `Level` declares 1, 2, 3, so
// `default(Level)` is a compile error (§17.11) and the guarantee cannot hold —
// so the INSTANTIATION is rejected, at the call site, while the body itself
// still type-checks "under the weakest assumption its constraints establish".
//
// This is a live producer of the value §5.4's bolded invariant forbids — and
// that invariant is what §19.6's exhaustiveness guarantee and §21.7's totality
// grant rest on. WRONG-without: compiles, and `U.Z<Level>()` yields `Level(0)`.

module M;

enum Level Low := 1, Mid := 2, High := 3; end Level;

static class U
  public static method Z<T>(): T where T: value var t := default(T); return t; end Z;
end U;

class C public static method Main(): int var l := U.Z<Level>(); return int(l); end Main; end C;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: type argument `Level` does not satisfy the constraint `where T: value`
// CHECK-SAME: takes `default(T)`
