//! case: s27_6_2230
//! doc: spec
//! section: 27.6
//! title: a generic overload first does not make the reference ambiguous;
//!   the formed Len() renders as its type
//! rule: a lambda and a method reference render alike, through `Object`
//! cites: 27.6, 14.5, 8.5
//! expect: run
//! stdout: () -> int
//! notes: Trace: the generic overload is excluded, leaving one candidate,
//!   so the reference forms () -> int, and a function value renders as
//!   its function type (8.5). The pre-fix compiler (79accd22) rejects
//!   this program because it counts the generic overload as a
//!   candidate; it passes on the staging tip.
module Conf;

class V
  public constructor() end constructor;
  public method Len<T>(x: T): int => 7;
  public method Len(): int => 42;
end V;

public class Program
  public static method Main()
    var v := new V();
    Console.WriteLine(v.Len);
  end Main;
end Program;
