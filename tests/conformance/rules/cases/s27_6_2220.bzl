//! case: s27_6_2220
//! doc: spec
//! section: 27.6
//! title: a (int) -> int target selects Len(int) although a generic
//!   overload is declared first
//! rule: without one, only candidates declaring none take part, and the
//!   error above is reported where every candidate is generic
//! cites: 27.6, 14.5
//! expect: run
//! stdout: 105
//! notes: Trace: the generic overload is excluded from a list-less
//!   reference (27.6), the (int) -> int target unifies with Len(int)
//!   exactly, f(5) = 5 + 100 = 105.
module Conf;

class V
  public constructor() end constructor;
  public method Len<T>(x: T): int => 7;
  public method Len(): int => 42;
  public method Len(x: int): int => x + 100;
end V;

public class Program
  public static method Main()
    var v := new V();
    var f: (int) -> int := v.Len;
    Console.WriteLine(f(5));
  end Main;
end Program;
