//! case: s9_7_6492
//! doc: spec
//! section: 9.7
//! title: runtime const records rebuilt on every pass of a while loop each
//!   read their string field
//! rule: runtime-managed values have unbounded lifetime by construction
//! cites: 9.7, 21.1
//! expect: run
//! stdout: 2070
module Conf;
record Name(value: string, n: int);
class C
  static method Once(i: int): int
    var s := 0; var j := 0; while j < 3 do const t := "x" + i.ToString(); const n := new Name(t, j); s := s + n.value.Length; j := j + 1; end; return s;
  end Once;
  public static method Main(): int
    var t := 0;
    for var i in 0..<200 do t := t + Once(i); end;
    Console.WriteLine(t);
    return 0;
  end Main;
end C;
