// fix #9171 (codegen; language spec §22.3 *Parameter modifiers* / §22.3 *`ref`
// and `out` argument locations* / §28.3.8 *Borrow lifetime rules* / §27.1
// *Function types*, v0.359): a DIRECT CALL of a function-typed `ref`
// PARAMETER — `x()` where the callee declares `ref x: () -> int` — did not
// lower at all. The compiler printed only the #2491 `internal:` return-degrade
// note
//
//   internal: return-value expression failed to lower to a value (degraded to
//   a zero/null return value) — #2491
//
// and the method silently returned 0, so the program printed a wrong value.
//
// §22.3: a `ref` parameter "aliases a location the caller chose" and "is read
// at its declared type", so the function value it names is the CONTENT of that
// location; §28.3.8: "Where the location's declared type is a function type it
// is also a function-typed holder." Invoking it is an ordinary call.
//
// THIS FILE IS THE DIAGNOSTIC ORACLE.  It has to be lit rather than gtest: the
// AOT gtest harness populates `JitResult::diagnostics` only on a compile
// REJECT, so an "assert no `internal:` note" written there passes VACUOUSLY on
// any compiler, fixed or not.  That was measured and recorded by #2963 — see
// tests/codegen/call_lowered_as_unit_vs_declined_2963_test.cpp.  The runtime
// half — that the call really did lower, and invoked the caller's closure —
// is pinned separately in
// tests/ownership/ref_function_param_direct_call_9171_test.cpp.
//
// THREE INDEPENDENT ROUTES, so this cannot pass by one arm having been
// special-cased:
//   * `Peek(ref x: () -> int): int => x();`   — the ARROW body (the issue's
//     verbatim repro);
//   * `Peek(ref x: () -> int): int return x(); end Peek;` — the BLOCK body;
//   * `Apply<T>(ref x: (T) -> T, v: T): T`     — the GENERIC sibling.
//
// Clean-compile ACCEPT control: bezelc -c must exit zero and emit no
// internal: / #2491 diagnostics.
//
// RUN: %bezelc -c -o %t.o %s
// RUN: %bezelc -c %s 2>&1 | FileCheck --check-prefix=NODIAG --allow-empty %s

module M;
import System;
public class Program
  public static method Peek(ref x: () -> int): int => x();
  public static method Block(ref x: () -> int): int return x(); end Block;
  public static method Apply<T>(ref x: (T) -> T, v: T): T
    return x(v);
  end Apply;
  public static method C(p: () -> int): int return Peek(ref p); end C;
  public static method Main(): int
    var sum := 0;
    for var i in 0 ..< 200 do
      sum := sum + C(lambda() => 1);
    end for;
    Console.WriteLine(sum);
    return 0;
  end Main;
end Program;

// NODIAG-NOT: error
// NODIAG-NOT: warning
// NODIAG-NOT: internal:
// NODIAG-NOT: #2491
