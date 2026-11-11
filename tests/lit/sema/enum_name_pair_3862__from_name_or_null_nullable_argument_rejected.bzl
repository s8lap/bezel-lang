// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// fix #3862 (§10.4, v0.357; stdlib §9.9): "the parameter is non-nullable, so
// a `string?` in hand is narrowed first (`s!`, or a null test)" (stdlib
// §9.9). §7.1: "The compiler enforces the distinction at every assignment,
// parameter pass, and return." An un-narrowed `string?` argument therefore
// reaches no `string` parameter.
//
// The argument is UN-narrowed: `Maybe()` returns `string?`, so the
// declaration establishes no §7.4 fact (§7.4's "from a declaration whose
// initializer is such an `expr`" — the initializer's static type is
// nullable). A §7.4-narrowed read is of its non-nullable type and is
// admitted.

module M;

enum Color Red, Green; end Color;

class C
  public static method Maybe(): string? => null;
  public static method Main(): int
    var s := C.Maybe();
    var p := Color.FromNameOrNull(s);
    return (if p = null then 1 else 0);
  end Main;
end C;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: the argument to `Color.FromNameOrNull` is nullable but the target type is non-nullable
