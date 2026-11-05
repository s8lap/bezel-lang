//! case: s27_6_2212
//! doc: spec
//! section: 27.6
//! title: an overloaded reference bound with var in an invoked lambda under
//!   an extension call is reported
//! rule: When no target type is available (e.g., `var f := Math.Abs;` with
//!   no annotation), the reference is ambiguous and the compiler
//!   reports an error pointing to the candidate overloads.
//! cites: 27.3, 26.5
//! expect: reject
//! diagnostic: no target type
//! notes: Exactly 1 error on the staging tip, all the quoted one; the
//!   accepting twin (the reference replaced by a call, or the
//!   ambiguity removed) compiles.
module Conf;

class V
  public static method Len(): int => 42;
  public static method Len(x: int): int => x + 100;
end V;
extension Object
  public method Where(n: int): int => n;
end;

public class Program
  public static method Main()
    using var o := new Object();
    Console.WriteLine(o.Where((lambda() => do var f := V.Len; return 1; end)()));
  end Main;
end Program;
