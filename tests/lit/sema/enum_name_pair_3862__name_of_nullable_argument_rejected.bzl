// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// fix #3862 (§10.4, v0.357; stdlib §9.9): §10.4 declares
// `public static method NameOf(value: E): string` — the parameter is the ENUM
// TYPE, and it is NON-NULLABLE. §7.1: "The compiler enforces the distinction
// at every assignment, parameter pass, and return." An un-narrowed `E?`
// argument therefore reaches no `E` parameter.
//
// `E` is a VALUE type (§7.2): its nullable form is the tagged-optional
// carrier, so the read at this position would hand the callee a
// `{hasValue, value}` slot where a bare `E` is expected — the shape §7.1
// exists to prevent.
//
// The argument is UN-narrowed: `Maybe()` returns `Color?`, so the declaration
// establishes no §7.4 fact. A §7.4-narrowed read is of its non-nullable type
// and is admitted.

module M;

enum Color Red, Green; end Color;

class C
  public static method Maybe(): Color? => null;
  public static method Main(): int
    var v := C.Maybe();
    var s := Color.NameOf(v);
    return 0;
  end Main;
end C;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: the argument to `Color.NameOf` is nullable but the target type is non-nullable
