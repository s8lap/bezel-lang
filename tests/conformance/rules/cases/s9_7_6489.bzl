//! case: s9_7_6489
//! doc: spec
//! section: 9.7
//! title: a runtime const record of two const string locals reads both fields
//! rule: runtime-managed values have unbounded lifetime by construction
//! cites: 9.7, 21.1
//! expect: run
//! stdout: 1380
module Conf;
record Pair(a: string, b: string);
class C
  static method Once(i: int): int
    const t := "x" + i.ToString(); const u := "y" + i.ToString(); const n := new Pair(t, u); return n.a.Length + n.b.Length;
  end Once;
  public static method Main(): int
    var t := 0;
    for var i in 0..<200 do t := t + Once(i); end;
    Console.WriteLine(t);
    return 0;
  end Main;
end C;
