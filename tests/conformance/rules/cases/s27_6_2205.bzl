//! case: s27_6_2205
//! doc: spec
//! section: 27.6
//! title: an overloaded reference bound with var inside a lambda nested in
//!   a lambda argument is reported
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
  public method Where(s: string): string => s;
  public method Where(n: int): int => n;
end;

public class Program
  public static method Main()
    using var o := new Object();
    Console.WriteLine(o.Where((lambda() => do return (lambda() => do var f := V.Len; return 1; end)(); end)()));
  end Main;
end Program;
