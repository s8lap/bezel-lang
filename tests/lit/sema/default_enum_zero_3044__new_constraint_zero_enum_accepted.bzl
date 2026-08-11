// fix #3044 (§17.11 / §14.2, v0.242) OVER-REJECTION GUARD: the instantiation
// check must fire ONLY for an enum with no zero-valued constant. An enum that
// declares one still satisfies `where T: new()` — its `default` is the declared
// zero constant (§17.11's enum row), exactly as through v0.241.
//
// Paired with default_enum_zero_3044__new_constraint_instantiation_rejected.bzl:
// same program, same generic, the ONLY difference is whether `Level` declares
// `Off := 0`.
//
// The negative CHECK is narrowed to the §14.2 constraint diagnostic rather than
// the usual blanket "no `§` at all" clean-compile control: this program's
// generic body reaches codegen, where the UNRELATED, PRE-EXISTING #2477/#2434
// dead-abstract-template-body notes for `new T()` (and §28.3's auto-drop lint
// on the template's `t`) are emitted for every `where T: new()` accept test.
// Asserting their absence would pin someone else's open gap, not this fix.
//
// RUN: %bezelc -c -o %t.o %s
// RUN: %bezelc -c %s 2>&1 | FileCheck --check-prefix=NODIAG --allow-empty %s

module M;

enum Level Off := 0, Low := 1, High := 2; end Level;

static class Utils public static method Make<T>(): int where T: new() var t := new T(); return 1; end Make; end Utils;

class C public static method Main(): int return Utils.Make<Level>(); end Main; end C;

// NODIAG-NOT: does not satisfy the constraint
// NODIAG-NOT: {{§}}17.11
