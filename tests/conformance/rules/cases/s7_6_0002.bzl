//! case: s7_6_0002
//! doc: spec
//! section: 7.6
//! title: argument expressions of a null-conditional call are not evaluated when null
//! rule: When the function-valued operand is null, the invocation does not
//!   occur and the argument expressions are not evaluated —
//!   `handler?(Next())` does not call `Next` when `handler` is null.
//! expect: run
//! stdout: done
module Conf;

public class Program
  public static method Next(): int
    Console.WriteLine("computed");
    return 1;
  end Next;

  public static method Main()
    var handler: ((int) -> ())? := null;
    handler?(Next());
    Console.WriteLine("done");
  end Main;
end Program;
