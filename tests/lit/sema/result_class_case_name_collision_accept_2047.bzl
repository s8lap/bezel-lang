// §35.5 / §19.3 (v0.200, #2047 follow-up): a user type whose NAME equals one of
// the auto-imported `Result` choice's case names (`Ok` / `Error`) must NOT break
// compilation. `Result` is injected into every unit and its own `IsOk` body
// matches `case this of Ok(_) => …`; because sema leaves `this` untyped for a
// named-type method, the pattern head `Ok` would fall through to general name
// lookup and collide with the user's `class Ok`, spuriously reporting
// "positional destructuring pattern `Ok(...)` is not valid on a class type
// (§19.3)" INSIDE the prelude. Per §35.5 "a case name resolves to a case
// pattern, never a type test" (spec line: "The same omission applies within a
// `case` over a choice scrutinee … a case name resolves to a case pattern, never
// a type test"), the case-scrutinee-first resolution must apply even for `this`.
//
// This is the shape that caught the defect (a user `class Ok` alongside any use
// — even none — of the injected Result), as a clean-compile ACCEPT.
//
// RUN: %bezelc -c -o %t.o %s
// RUN: %bezelc -c %s 2>&1 | FileCheck --check-prefix=NODIAG --allow-empty %s

module M;

record Ev(N: int);
class Ok public event Boom: Ev; end Ok;   // NAME collides with Result's Ok case

class Error                                // and with Result's Error case
  public code: int;
  public constructor(c: int) code := c; end constructor;
end Error;

// NODIAG-NOT: error
// NODIAG-NOT: §
