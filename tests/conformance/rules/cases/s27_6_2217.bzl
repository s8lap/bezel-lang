//! case: s27_6_2217
//! doc: spec
//! section: 27.6
//! title: a generic overload does not satisfy a function-typed target; the
//!   lone non-generic one does not unify
//! rule: without one, only candidates declaring none take part, and the
//!   error above is reported where every candidate is generic
//! cites: 27.6, 5.1
//! expect: reject
//! diagnostic: target
//! notes: Exactly 1 error on the staging tip, all the quoted one; the
//!   accepting twin (the reference replaced by a call, or the
//!   ambiguity removed) compiles.
module Conf;

class V
  public constructor() end constructor;
  public method Len<T>(x: T): int => 7;
  public method Len(x: string): int => 9;
end V;

public class Program
  public static method Main()
    var v := new V();
    var f: (int) -> int := v.Len;
    Console.WriteLine(f(5));
  end Main;
end Program;
