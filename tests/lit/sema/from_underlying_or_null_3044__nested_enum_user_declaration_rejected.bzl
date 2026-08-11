// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// fix #3044 (§10.4, v0.242): §10.4's "no user declaration may override or
// overload it" applies to EVERY enum, nested ones included. The sibling pin
// from_underlying_or_null_3044__user_declaration_rejected.bzl covers the
// module-level case; this one covers the nested case, which the §10.5
// restriction walk (module children only) does not reach.
//
// The nested case is the one where silence would be worst: both
// `ExprTC::enumTypeNamedBy` and `Emitter::tryLowerEnumFromUnderlyingOrNull`
// resolve a NestedTypeSymbol, so an accepted user declaration here would be
// shadowed by the compiler-provided member at every call site — the exact
// outcome §10.4's reject exists to prevent.

module M;

class Outer
  public enum Inner Red, Green;
    public static method FromUnderlyingOrNull(value: int): Inner? => null;
  end Inner;
end Outer;

class C public static method Main(): int return 0; end Main; end C;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: enum `Inner` may not declare a member named `FromUnderlyingOrNull`
// CHECK-SAME: (§10.4)
