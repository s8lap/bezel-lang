// fix #3044 (§17.11, v0.242) OVER-REJECTION GUARD: the instantiation-site check
// is gated on `where T: value` and must NOT extend to an UNCONSTRAINED `T`.
// §17.11's generic-body list is explicit about the difference:
//
//   "For unconstrained `T`, `default(T)` is permitted but its result is treated
//    as possibly null and cannot be assigned to a non-nullable T-typed location."
//   "Under `where T: value`, `default(T)` is always non-null."
//
// Only the second is a guarantee a not-well-defined default can contradict. An
// unconstrained `default(T)` instantiated at `string` — whose `default` is NOT
// well-defined (§17.11: `string` is a non-nullable reference type) — is
// therefore legal and must stay legal; rejecting it would be the over-broad
// reading.
//
// The body deliberately does not RETURN `T`: a static method returning a
// possibly-class `T` needs `owned` (§28.3 factory rule), which is an unrelated
// obligation this guard has no business exercising.
//
// Clean-compile ACCEPT control: bezelc -c must exit zero and emit no diagnostics.
//
// RUN: %bezelc -c -o %t.o %s
// RUN: %bezelc -c %s 2>&1 | FileCheck --check-prefix=NODIAG --allow-empty %s

module M;

static class U
  public static method Z<T>(): int var t := default(T); return 7; end Z;
end U;

class C public static method Main(): int return U.Z<string>(); end Main; end C;

// NODIAG-NOT: error
// NODIAG-NOT: §
