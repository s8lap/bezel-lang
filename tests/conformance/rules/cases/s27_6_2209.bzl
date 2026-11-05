//! case: s27_6_2209
//! doc: spec
//! section: 27.6
//! title: an untargeted overloaded reference in a generic method body is
//!   rejected
//! rule: When no target type is available (e.g., `var f := Math.Abs;` with
//!   no annotation), the reference is ambiguous and the compiler
//!   reports an error pointing to the candidate overloads.
//! cites: 14.1
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

public class Program
  public static method Gen<T>(x: T): int
    var f := V.Len;
    return 1;
  end Gen;
  public static method Main()
    Console.WriteLine(Program.Gen<int>(1));
    Console.WriteLine(Program.Gen<string>("a"));
  end Main;
end Program;
