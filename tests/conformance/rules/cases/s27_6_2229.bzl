//! case: s27_6_2229
//! doc: spec
//! section: 27.6
//! title: an explicit type-argument list selects only the generic overload:
//!   Len<int> is applied
//! rule: with a list, only candidates declaring that many type parameters
//!   of their own take part
//! cites: 27.6, 14.5
//! expect: run
//! stdout: 7
//! notes: Trace: the explicit list names one type argument, so only the
//!   generic overload takes part, bound at int; f(5) = 7. The pre-fix
//!   compiler (79accd22) rejects this program because it counts the
//!   generic overload as a candidate; it passes on the staging tip.
module Conf;

class V
  public constructor() end constructor;
  public method Len<T>(x: T): int => 7;
  public method Len(): int => 42;
end V;

public class Program
  public static method Main()
    var v := new V();
    var f := v.Len<int>;
    Console.WriteLine(f(5));
  end Main;
end Program;
