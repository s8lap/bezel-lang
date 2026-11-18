//! case: s9_7_6494
//! doc: spec
//! section: 9.7
//! title: a runtime const record returned from its declaring method keeps its
//!   string field readable
//! rule: runtime-managed values have unbounded lifetime by construction
//! cites: 9.7, 21.1
//! expect: run
//! stdout: 890
module Conf;
record Name(value: string, n: int);
class C
  static method Mk(i: int): Name
    const t := "x" + i.ToString(); const n := new Name(t, 1); return n;
  end Mk;
  static method Once(i: int): int
    var r := Mk(i); return r.value.Length + r.n;
  end Once;
  public static method Main(): int
    var t := 0;
    for var i in 0..<200 do t := t + Once(i); end;
    Console.WriteLine(t);
    return 0;
  end Main;
end C;
