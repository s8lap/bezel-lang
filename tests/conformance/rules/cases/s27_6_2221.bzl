//! case: s27_6_2221
//! doc: spec
//! section: 27.6
//! title: a () -> int target selects Len() although a generic overload is
//!   declared first
//! rule: without one, only candidates declaring none take part, and the
//!   error above is reported where every candidate is generic
//! cites: 27.6, 14.5
//! expect: run
//! stdout: 42
//! notes: Trace: the () -> int target selects Len() exactly, f() = 42.
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
    var f: () -> int := v.Len;
    Console.WriteLine(f());
  end Main;
end Program;
