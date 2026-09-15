// issue #4178 — the return-type shape from the issue's own repro. Pins TWO
// regressions at once, both now fixed:
//
//   1. Naming `IOrderedEnumerable<T>` as an explicit return-type annotation
//      used to fail "unresolved type name `IOrderedEnumerable`"
//      (`Resolver::builtinTypeArity`, TypeResolution_Names.cpp).
//   2. Once (1) alone is fixed but the type's LLVM representation is not
//      also registered, a function whose declared return type is
//      `IOrderedEnumerable<T>` IR-verify-fails at codegen ("Function return
//      type does not match operand type of return inst!") because
//      `lowerTypeRef`'s Named case falls through to the i32 default instead
//      of the `ptr` every other lazy-query-shaped builtin (IEnumerable,
//      IGrouping) already gets
//      (`kHeapPointerReferenceBuiltins`, BuiltinRegistry.cpp).
//
// The first compile line below alone is sufficient to catch (2): `-c`
// drives full LLVM codegen + IR verification, not just parsing/sema.
//
// Spec authority: stdlib §5 `OrderBy<TKey>(key: (T) -> TKey):
// IOrderedEnumerable<T> where TKey: IComparable<TKey>;` — a helper method
// returning the operator's own declared result type is spec-legal.
//
// RUN: %bezelc -c -o %t.o %s
// RUN: %bezelc -c %s 2>&1 | FileCheck --check-prefix=NODIAG --allow-empty %s

module Probe; public class C public static method Foo(xs: Array<int>): IOrderedEnumerable<int> => xs.OrderBy(lambda(x) => x); public static method Main(): int return 0; end Main; end C;

// NODIAG-NOT: error
// NODIAG-NOT: unresolved type name
// NODIAG-NOT: IR verification failed
