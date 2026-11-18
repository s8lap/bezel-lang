//! case: s9_7_6495
//! doc: spec
//! section: 9.7
//! title: a runtime const record captured by a returned lambda keeps its
//!   string field readable
//! rule: runtime-managed values have unbounded lifetime by construction
//! cites: 9.7, 21.1, 27.5
//! expect: run
//! stdout: 690
module Conf;
record Name(value: string, n: int);
class C
  static method MkF(i: int): () -> int
    const t := "x" + i.ToString(); const n := new Name(t, 1); return lambda() => n.value.Length;
  end MkF;
  static method Once(i: int): int
    var f := MkF(i); return f();
  end Once;
  public static method Main(): int
    var t := 0;
    for var i in 0..<200 do t := t + Once(i); end;
    Console.WriteLine(t);
    return 0;
  end Main;
end C;
