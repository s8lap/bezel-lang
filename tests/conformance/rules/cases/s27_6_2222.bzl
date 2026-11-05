//! case: s27_6_2222
//! doc: spec
//! section: 27.6
//! title: a generic overload declared first does not take part in a
//!   targeted reference: Len(int) is selected
//! rule: without one, only candidates declaring none take part, and the
//!   error above is reported where every candidate is generic
//! cites: 27.6, 14.5
//! expect: run
//! stdout: 105
//! notes: Trace: the generic overload is excluded, the (int) -> int target
//!   selects Len(int), f(5) = 105.
module Conf;

class V
  public constructor() end constructor;
  public method Len<T>(x: T): int => 7;
  public method Len(x: int): int => x + 100;
end V;

public class Program
  public static method Main()
    var v := new V();
    var f: (int) -> int := v.Len;
    Console.WriteLine(f(5));
  end Main;
end Program;
