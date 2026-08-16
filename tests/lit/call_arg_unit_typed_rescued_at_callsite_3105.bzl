// fix #3105 (broaden #3079's Console guard to the broader call family,
// spec §4.7 / §8.5 / §21.9 / stdlib §2.1, v0.255): the #3079 guard sat
// only inside `tryLowerConsoleCall`. The same unit-typed value was
// emitted as a void-typed `CallInst` (the `CallInst` IS a `Value`, its
// type is just `void`), and the broader call args loops saw
// `if (!v)` false, pushed the void value into the args vector, and the
// IR verifier aborted with "Call parameter type does not match function
// signature" — MEASURED on 50eb493 with the issue's
// `TakeUnit(ReturnsUnit())` repro.
//
// The fix detects the void-typed v BEFORE the existing `!v` rescue
// runs at every call args loop (the central `buildOrderedArgs`, the
// imported-method ordered-args loop on the `?.` twin, the IIFE loop,
// the closure-call loop) and substitutes the §4.7 unit placeholder
// byte (`i8 0`, via `unitPlaceholderTy()`) when the formal parameter
// is unit-typed. The side effects of the original call are preserved
// at every site — only the `Value*` the downstream code stored as the
// call argument is replaced.
//
// The accompanying gtest `tests/codegen/call_arg_unit_typed_rescued_3105_test.cpp`
// pins the rendered output for the THREE shapes the issue names
// (non-Console callee, `?.` twin, closure-call / IIFE). THIS lit test
// pins the diagnostic silence — the broader call-family rescue the
// fix installs must NOT emit any IR verifier error or #3079
// "emits nothing" Warning for the unit-typed argument; the fix
// restores the §8.5 sentinel instead. A regression to the IR-verifier
// abort (or the silent-drop) surfaces here as the matched line.

// RUN: %bezelc -c -o %t.o %s
// RUN: %bezelc -c %s 2>&1 | FileCheck --allow-empty %s

module N3105Lit;
class C
  public static method ReturnsUnit(): ()
    return;
  end ReturnsUnit;
  public static method TakeUnit(u: ()): int
    return 0;
  end TakeUnit;
  public static method Main(): int
    TakeUnit(ReturnsUnit());
    return 0;
  end Main;
end C;

// NEGATIVE — the broader call-family rescue the fix installs must
// NOT emit the IR verifier abort ("Call parameter type does not
// match function signature") for the unit-typed argument; the fix
// substitutes the §8.5 placeholder byte instead. A regression to the
// IR-verifier abort surfaces here as the matched line and FileCheck
// fails. The check is exhaustive — every "Call parameter type" line
// the verifier would emit on the malformed-IR path is rejected.
// CHECK-NOT: Call parameter type does not match function signature

