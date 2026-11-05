//! case: s27_6_2215
//! doc: spec
//! section: 27.6
//! title: an untargeted reference with two same-arity non-generic
//!   candidates is ambiguous
//! rule: without one, only candidates declaring none take part, and the
//!   error above is reported where every candidate is generic
//! cites: 27.6, 14.5
//! expect: reject
//! diagnostic: candidates
//! notes: Exactly 1 error on the staging tip, all the quoted one; the
//!   accepting twin (the reference replaced by a call, or the
//!   ambiguity removed) compiles.
module Conf;

class V
  public constructor() end constructor;
  public method Len<T>(x: T): int => 7;
  public method Len(x: int): int => x + 100;
  public method Len(x: string): int => 9;
end V;

public class Program
  public static method Main()
    var v := new V();
    var f := v.Len;
    Console.WriteLine(1);
  end Main;
end Program;
