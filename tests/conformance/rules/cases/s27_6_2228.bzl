//! case: s27_6_2228
//! doc: spec
//! section: 27.6
//! title: a generic overload first does not defeat a method-group argument
//!   to Select: Len(int) is applied
//! rule: without one, only candidates declaring none take part, and the
//!   error above is reported where every candidate is generic
//! cites: 27.6, 14.5
//! expect: run
//! stdout: 306
//! notes: Trace: Select's (int) -> TKey target selects Len(int) for the
//!   group argument; the elements 1, 2, 3 map to 101, 102, 103, whose
//!   sum is 306. The pre-fix compiler (79accd22) rejects this program
//!   because it counts the generic overload as a candidate; it passes
//!   on the staging tip.
module Conf;

class V
  public constructor() end constructor;
  public method Len<T>(x: T): int => 7;
  public method Len(x: int): int => x + 100;
end V;

public class Program
  public static method Main()
    var v := new V();
    var a: Array<int> := [1, 2, 3];
    var r := a.Select(v.Len).ToArray();
    Console.WriteLine(r[0] + r[1] + r[2]);
  end Main;
end Program;
