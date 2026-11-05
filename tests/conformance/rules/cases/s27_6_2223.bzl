//! case: s27_6_2223
//! doc: spec
//! section: 27.6
//! title: one non-generic candidate beside a generic overload: the
//!   untargeted reference forms (int) -> int
//! rule: without one, only candidates declaring none take part, and the
//!   error above is reported where every candidate is generic
//! cites: 27.6, 14.5
//! expect: run
//! stdout: 105
//! notes: Trace: the generic overload is excluded, leaving one non-generic
//!   candidate, so the untargeted reference is not ambiguous and forms
//!   (int) -> int, f(5) = 105. The pre-fix compiler (79accd22) rejects
//!   this program because it counts the generic overload as a
//!   candidate; it passes on the staging tip.
module Conf;

class V
  public constructor() end constructor;
  public method Len<T>(x: T): int => 7;
  public method Len(x: int): int => x + 100;
end V;

public class Program
  public static method Main()
    var v := new V();
    var f := v.Len;
    Console.WriteLine(f(5));
  end Main;
end Program;
