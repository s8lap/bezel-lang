// §22.3 / §14.5 (spec v0.228) / #2266 over-rejection control: a generic method
// whose ref parameter is an INLINE ARRAY OF the method type parameter
// (`ref r: [4]T`). The type parameter is nested in the TypeRef's arrayElement,
// not in genericArgs — the shape the earlier #1756 attempts' narrow guards
// (containsBareTypeParam / containsUnresolvedTypeParam, both recursing only
// genericArgs) failed to recognise, so they ran the exact comparison against
// the unsubstituted `[4]T` and false-rejected a matching `[4]int` argument.
// The per-parameter skip must walk arrayElement (and function parameter/return
// slots) too, leaving this call to §14.5.
//
// Clean-compile ACCEPT control: bezelc -c must exit zero and emit no diagnostics.
//
// RUN: %bezelc -c -o %t.o %s
// RUN: %bezelc -c %s 2>&1 | FileCheck --check-prefix=NODIAG --allow-empty %s

module M; static class U public static method Fill<T>(v: T, ref r: [4]T) r[0] := v; end Fill; end U; class C public static method Main(): int var arr: [4]int; U.Fill<int>(7, ref arr); return arr[0]; end Main; end C;

// NODIAG-NOT: error
// NODIAG-NOT: §
