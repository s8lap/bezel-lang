//! case: s27_6_2203
//! doc: spec
//! section: 27.6
//! title: two overloaded references bound with var inside an invoked lambda
//!   argument are both reported
//! rule: When no target type is available (e.g., `var f := Math.Abs;` with
//!   no annotation), the reference is ambiguous and the compiler
//!   reports an error pointing to the candidate overloads.
//! cites: 27.3, 26.5
//! expect: reject
//! diagnostic: no target type
//! notes: Exactly 2 errors (one per reference) on the staging tip, all the
//!   quoted one; the accepting twin (the reference replaced by a call,
//!   or the ambiguity removed) compiles.
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
    Console.WriteLine(o.Where((lambda() => do var f := V.Len; var g := V.Len; return 1; end)()));
  end Main;
end Program;
