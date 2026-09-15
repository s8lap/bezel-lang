// fix #4352 (§28.2 / §28.3, v0.298): `return x;` after an explicit
// `x.Dispose();` is a use-after-free. The explicit `.Dispose()` frees the
// object and nulls the slot; `return x;` then reads the null/dangling
// slot and returns a freed pointer. ASan confirms: SEGV at offset 12
// (null field access) on the unfixed build.
//
// The explicit double-dispose `x.Dispose(); x.Dispose();` is still a
// Warning (idempotent, §28.2) — the first `.Dispose()` is legal and the
// second is a no-op warning. But `return x;` after `x.Dispose();` is NOT
// a second dispose: it is a USE of the already-disposed variable, and must
// be a hard Error.
//
// Acceptance control: the unfixed compiler accepts this program (warning only
// for the second explicit `.Dispose()`, `return x;` is silent). The fixed
// compiler rejects it with an Error at the `return` site.
//
// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s

module M4352;

// The explicit double-dispose shape: `x.Dispose(); x.Dispose();` must
// still produce only a Warning (not an Error), confirming §28.2's idempotent
// guarantee is preserved.
class Box
  public n: int;
  public constructor(nn: int) this.n := nn; end constructor;
  destructor end destructor;
end Box;

class C
  public static method Test1(): int
    var b := new Box(1);
    b.Dispose();
    b.Dispose();  // redundant second dispose: Warning (§28.2)
    return 0;
  end Test1;

  // THE BUG: `return b;` after `b.Dispose();` silently accepted (no Error)
  // in the unfixed compiler, producing a use-after-free. The fix adds a hard
  // Error at the `return` site.
  public static method Test2(): owned Box
    var b := new Box(42);
    b.Dispose();
    return b;  // CHECK: local `b` is already disposed
  end Test2;

  public static method Main(): int
    return 0;
  end Main;
end C;
