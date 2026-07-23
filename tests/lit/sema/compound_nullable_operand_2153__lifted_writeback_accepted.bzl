// #2153 (§6.6 / §7.11 / §5.1, spec v0.220) — ACCEPT control: the lifted
// admissible cells must stay admitted when the family check is extended to
// nullable operands.
//   - `int? += long`  — §7.11 mixed lift; §6.6's checked narrow-integer
//     write-back applies on the present branch (#1964/#2140 established;
//     `byte? += 100` raising OverflowException is pinned by the #1964 gtest).
//   - `int? += long?` — both-nullable lift, same write-back.
//   - `int? &= long?` — bitwise lifts over nullable INTEGER operands (§7.11).
//   - `decimal? += int?` — §5.1 int→decimal widening under the lift (#2149's
//     landed cell, non-regression anchor).
//   - `char? += 1` — char target: §5.2 code-point write-back (#1964's cell).
//   - `byte? += 1` — constant RHS on a sub-int nullable target.
//
// Clean-compile ACCEPT control: bezelc -c must exit zero and emit no
// diagnostics.
//
// RUN: %bezelc -c -o %t.o %s
// RUN: %bezelc -c %s 2>&1 | FileCheck --check-prefix=NODIAG --allow-empty %s

module M;
class C
  public static method Main(): int
    var a: int? := 5;
    a += 10L;
    var b: int? := 5;
    var w: long? := 20L;
    b += w;
    var m: int? := 12;
    var n: long? := 10L;
    m &= n;
    var d: decimal? := 1.5m;
    var i: int? := 2;
    d += i;
    var c: char? := char(65);
    c += 1;
    var y: byte? := 100;
    y += 1;
    return 0;
  end Main;
end C;

// NODIAG-NOT: error
// NODIAG-NOT: §
