//! case: s27_6_2210
//! doc: spec
//! section: 27.6
//! title: an untargeted overloaded reference in a generic class body is
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
class Box<T>
  public constructor() end constructor;
  public method M(): int
    var f := V.Len;
    return 1;
  end M;
end Box;

public class Program
  public static method Main()
    using var a := new Box<int>();
    using var b := new Box<string>();
    Console.WriteLine(a.M() + b.M());
  end Main;
end Program;
