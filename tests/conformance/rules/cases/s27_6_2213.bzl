//! case: s27_6_2213
//! doc: spec
//! section: 27.6
//! title: an overloaded reference passed to an Object parameter has no
//!   function-typed target and is reported
//! rule: When no target type is available (e.g., `var f := Math.Abs;` with
//!   no annotation), the reference is ambiguous and the compiler
//!   reports an error pointing to the candidate overloads.
//! cites: 22.6, 26.5
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
class U
  public static method Show(x: Object): int => 3;
end U;
extension Object
  public method Where(n: int): int => n;
end;

public class Program
  public static method Main()
    using var o := new Object();
    Console.WriteLine(o.Where(U.Show(V.Len)));
  end Main;
end Program;
