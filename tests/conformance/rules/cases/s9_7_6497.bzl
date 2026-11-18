//! case: s9_7_6497
//! doc: spec
//! section: 9.7
//! title: a runtime const record stored by a callee into the caller's array
//!   keeps its string field readable
//! rule: runtime-managed values have unbounded lifetime by construction
//! cites: 9.7, 21.1
//! expect: run
//! stdout: 690
module Conf;
record Name(value: string, n: int);
class C
  static method Stash(a: Array<Name>, p: Name) a.Add(p); end Stash;
  static method Once(i: int): int
    const t := "x" + i.ToString(); const n := new Name(t, 1); var a := new Array<Name>(); Stash(a, n); return a[0].value.Length;
  end Once;
  public static method Main(): int
    var t := 0;
    for var i in 0..<200 do t := t + Once(i); end;
    Console.WriteLine(t);
    return 0;
  end Main;
end C;
