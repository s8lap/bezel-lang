// Regression for #7843 (§28.3 *Auto-drop (affine)*, §28.10 *The anonymous
// binding*, §17.13 axiom A6, v0.355).
//
// The §28.3 must-use warning for an out-owned RECIPIENT local — a
// pre-declared `var it: Item? := null;` passed as a plain `out it` against an
// `out owned` formal (§22.4) — names the remedies that discharge it. Two
// things were wrong with that remedy half:
//
//   1. it spelled the adoption with the RETIRED `owned` binding marker:
//      "`using var _ := owned item`". `owned` stopped being the
//      binding-position marker at v0.286 (`move` replaced it), so the warning
//      told the user to write code that does not compile. #7791 fixed the
//      PARAMETER sites; this out-owned RECIPIENT site was missed.
//
//   2. it named a hard-coded `item` — not the local the warning's own first
//      clause names ("owned value in `it` …"). The other remedies in the same
//      list are written for THAT local, so the list must name it too.
//
// The remedy now reads `using var _ := move it;` — the shape the sibling
// parameter-site remedy uses (`adoptionRemedyText`): the ANONYMOUS binding
// for an ordinary value, and the NAMED `using var t2 := move t;` over a
// `Task`-family value, for which the anonymous binding is itself a compile
// error (§28.10: "A `Task`-family value may not be bound this way (§17.13,
// axiom A6)"). The Task-family recipient is REACHABLE: an `out owned t: Task?`
// formal is admitted (`Task` is a class type, §28.1 *Type requirement*) and
// the recipient adopts owned like any other.
//
// FAIL-WITHOUT / PASS-WITH: before the fix every line below read
// "`using var _ := owned item`" and "`item?.Dispose()`", so the CHECK-DAG
// lines did not match and the STALE guard fired.
//
// RUN: %bezelc -c -o %t.o %s
// RUN: %bezelc -c %s 2>&1 | FileCheck %s
// RUN: %bezelc -c %s 2>&1 | FileCheck --check-prefix=STALE %s

module M;
class Item destructor end destructor; end Item;
class Mk
  public static method TryGet(out owned it: Item?): bool
    it := new Item();
    return true;
  end TryGet;
  public static async method Work(): Task
    Console.WriteLine("w");
  end Work;
  public static method TryTask(out owned t: Task?): bool
    t := Work();
    return true;
  end TryTask;
end Mk;
class C
  public static method Main(): int
    var it: Item? := null;
    var ok := Mk.TryGet(out it);
    var t: Task? := null;
    var ok2 := Mk.TryTask(out t);
    return 0;
  end Main;
end C;

// CHECK-DAG: warning: owned value in `it` is implicitly disposed at the end of its scope (§28.3 must-use / auto-drop); use `it?.Dispose()`, `move`, `using var _ := move it;`, or `return` to discharge it explicitly (§28.3/§28.11)
// CHECK-DAG: warning: owned value in `t` is implicitly disposed at the end of its scope (§28.3 must-use / auto-drop); use `t?.Dispose()`, `move`, `using var t2 := move t;` (the anonymous `using var _` is rejected for a `Task`, §17.13), or `return` to discharge it explicitly (§28.3/§28.11)
// STALE-NOT: using var _ := owned
// STALE-NOT: use `item?.Dispose()`
