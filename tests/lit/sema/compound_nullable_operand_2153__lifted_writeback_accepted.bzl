// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s

// CHECK: compound assignment `+=` is not defined when the

// #2153 (§6.6 / §7.11 / §5.1, spec v0.220) — REJECTION override
// (#3298 / v0.267 / proposal 0114): the previously-admitted lifted
// form `int? += long` is rejected at the §6.6 gate (§7.11 lifting
// gone), as are all the other §6.6 compound forms in this file.
// The narrower write-back family (byte? += 100, char? += 1, …) used
// to compile because §7.11 lifted to T? then §6.6 narrowed via the
// checked-narrow-integer write-back; both rules are off now.
//

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
