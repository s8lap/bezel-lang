//! case: s27_6_2211
//! doc: spec
//! section: 27.6
//! title: an overloaded reference in an interpolation hole under an
//!   extension call is reported
//! rule: When no target type is available (e.g., `var f := Math.Abs;` with
//!   no annotation), the reference is ambiguous and the compiler
//!   reports an error pointing to the candidate overloads.
//! cites: 18.5, 26.5
//! expect: reject
//! diagnostic: no target type
//! issue: 8205
//! notes: Exactly 1 error on the staging tip, all the quoted one; the
//!   accepting twin (the reference replaced by a call, or the
//!   ambiguity removed) compiles. The pre-fix compiler (79accd22)
//!   compiles this program: it silently accepts the untargeted
//!   overloaded reference.
module Conf;

class V
  public static method Len(): int => 42;
  public static method Len(x: int): int => x + 100;
end V;
extension Object
  public method Where(s: string): string => s;
end;

public class Program
  public static method Main()
    using var o := new Object();
    Console.WriteLine(o.Where($"{V.Len}"));
  end Main;
end Program;
