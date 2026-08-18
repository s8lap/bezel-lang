// fix #3072 / §7.9 (v0.247) / §17.11 OVER-REJECTION GUARD: a class field of an
// enum type that DOES declare a zero-valued constant has a well-defined default
// (§17.11's enum row: "the value with underlying integer 0 — necessarily a
// declared constant") and is exempt from the new §7.9 carve-out. `Level`
// declares `Off := 0`, so `_l: Level` is a value-typed default-bearing field
// just like `_n: int` — it needs no initializer and no constructor assignment.
//
// Clean-compile ACCEPT control: bezelc -c must exit zero and emit no §7.9
// diagnostic. The DISPOSAL-stage §28.3 auto-drop lint on `c` is the program's
// pre-existing fate and not under test here — only the §7.9 carve-out's
// acceptance is.
//
// RUN: %bezelc -c -o %t.o %s
// RUN: %bezelc -c %s 2>&1 | FileCheck --check-prefix=NODIAG --allow-empty %s

module M;
enum Level Off := 0, Low := 1, Mid := 2; end Level;
class C
  private _l: Level;
  public method Read(): int => int(_l);
  public static method Main(): int
    using var c := new C();
    return c.Read();
  end Main;
end C;

// NODIAG-NOT: non-nullable field `_l`
// NODIAG-NOT: §7.9
// NODIAG-NOT: §17.11
