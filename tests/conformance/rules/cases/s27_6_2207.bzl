//! case: s27_6_2207
//! doc: spec
//! section: 27.6
//! title: an overloaded reference in a hole under a null-conditional call
//!   is reported
//! rule: When no target type is available (e.g., `var f := Math.Abs;` with
//!   no annotation), the reference is ambiguous and the compiler
//!   reports an error pointing to the candidate overloads.
//! cites: 18.5, 17.9
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

public class Program
  public static method Main()
    var s: string? := "x";
    var r := s?.IndexOf($"{V.Len}");
    Console.WriteLine(r ?? 0);
  end Main;
end Program;
