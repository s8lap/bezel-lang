//! case: s9_7_6490
//! doc: spec
//! section: 9.7
//! title: a runtime const record's string field interpolated into a string
//! rule: runtime-managed values have unbounded lifetime by construction
//! cites: 9.7, 21.1, 18.5
//! expect: run
//! stdout: 1090
module Conf;
record Name(value: string, n: int);
class C
  static method Once(i: int): int
    const t := "x" + i.ToString(); const n := new Name(t, 1); var s := $"{n.value}-{n.n}"; return s.Length;
  end Once;
  public static method Main(): int
    var t := 0;
    for var i in 0..<200 do t := t + Once(i); end;
    Console.WriteLine(t);
    return 0;
  end Main;
end C;
