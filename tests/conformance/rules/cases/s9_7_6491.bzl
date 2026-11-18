//! case: s9_7_6491
//! doc: spec
//! section: 9.7
//! title: a runtime const record's string field read in a try block returning
//!   through finally
//! rule: runtime-managed values have unbounded lifetime by construction
//! cites: 9.7, 21.1
//! expect: run
//! stdout: 690
module Conf;
record Name(value: string, n: int);
class C
  static method Once(i: int): int
    const t := "x" + i.ToString(); const n := new Name(t, 1); try return n.value.Length; finally Console.Write(""); end try;
  end Once;
  public static method Main(): int
    var t := 0;
    for var i in 0..<200 do t := t + Once(i); end;
    Console.WriteLine(t);
    return 0;
  end Main;
end C;
