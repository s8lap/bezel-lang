// fix #4200 (spec §10/§11, v0.298): a chained member call on a `new T()`
// receiver with a genuinely nonexistent member was silently accepted with zero
// diagnostics and exit 0.
//
// Root cause: `LlvmBackend_Calls2.cpp`'s end-of-chain fallthrough warning only
// fired when `isConcreteType` was true (receiver's TypeRef has a non-null
// `declaration`). `new Stopwatch()`'s receiver type resolves through
// `TypeResolution_Names.cpp`'s "nullary stdlib class" list with a null
// declaration, so `isConcreteType` was false and the warning never fired.
// By contrast, `Stopwatch.StartNew()`'s receiver type has a non-null declaration
// (via `builtinClassSym("Stopwatch")`), so `isConcreteType` is true and the
// identical bogus-member call IS correctly warned about.
//
// The fix adds a third shape to the fallthrough condition: call-shaped receiver
// (CallExpr or NewExpr) whose resolved type has no declaration, emitting the same
// "internal: codegen cannot dispatch member" warning.
//
// RUN: %bezelc -c -o %t.o %s
// RUN: %bezelc -c %s 2>&1 | FileCheck %s

module M;

import System.Diagnostics;

class C
  public static method Main(): int
    // The issue's own verbatim reproducer — must emit the warning.
    new Stopwatch().TotallyBogusNewChainedMember();
    return 0;
  end Main;
end C;

// CHECK: internal: codegen cannot dispatch member 'TotallyBogusNewChainedMember' on receiver of type 'Stopwatch'
// CHECK-NOT: TotallyBogusNewChainedMember
