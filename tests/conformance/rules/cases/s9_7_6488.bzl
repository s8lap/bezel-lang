//! case: s9_7_6488
//! doc: spec
//! section: 9.7
//! title: a runtime const record read by a callee in two calls, each reading
//!   its string field
//! rule: runtime-managed values have unbounded lifetime by construction
//! cites: 9.7, 21.1
//! expect: run
//! stdout: 1780
module Conf;
record Name(value: string, n: int);
class C
  static method Rd(p: Name): int => p.value.Length + p.n;
  static method Once(i: int): int
    const t := "x" + i.ToString(); const n := new Name(t, 1); return Rd(n) + Rd(n);
  end Once;
  public static method Main(): int
    var t := 0;
    for var i in 0..<200 do t := t + Once(i); end;
    Console.WriteLine(t);
    return 0;
  end Main;
end C;
