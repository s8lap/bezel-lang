//! case: s27_3_0915
//! doc: spec
//! section: 27.3
//! title: a nested lambda's returns are judged against its own result slot, not the outer one
//! rule: its inferred return type must equal the target's return slot or implicitly convert to it
//! cites: 27.4
//! expect: run
//! output: 7
//!   1
//! issue: 8168
module Conf;

public class Program
  public static method Main()
    var f: (long) -> int := lambda(x) => do
      var g: (long) -> long := lambda(y) => do
        return y;
      end;
      Console.WriteLine(g(7L));
      return 1;
    end;
    Console.WriteLine(f(3L));
  end Main;
end Program;
