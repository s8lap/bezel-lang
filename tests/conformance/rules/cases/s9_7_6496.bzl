//! case: s9_7_6496
//! doc: spec
//! section: 9.7
//! title: a runtime const record passed through a generic box and method
//!   keeps its string field readable
//! rule: runtime-managed values have unbounded lifetime by construction
//! cites: 9.7, 21.1
//! expect: run
//! stdout: 890
module Conf;
record Name(value: string, n: int);
class Box<T>
  public v: T;
  public constructor(x: T) v := x; end constructor;
end Box;
class C
  static method Keep<T>(x: T): T => x;
  static method Once(i: int): int
    const t := "x" + i.ToString(); const n := new Name(t, 1); var b := new Box<Name>(n); var k := Keep<Name>(n); return b.v.value.Length + k.n;
  end Once;
  public static method Main(): int
    var t := 0;
    for var i in 0..<200 do t := t + Once(i); end;
    Console.WriteLine(t);
    return 0;
  end Main;
end C;
