//! case: s28_3_8_3604
//! doc: spec
//! section: 28.3.8
//! title: a function-typed ref parameter's entry value may be invoked in the callee
//! rule: A read carrying the unannounced source may be invoked, bound to a local or passed at a plain
//!   function-typed parameter, but not stored into a holder (the `ref` or `out` location itself and a
//!   sibling one included), passed at a set-erasing position or returned from the enclosing method.
//! cites: 28.3.8, 22.3
//! expect: run
//! stdout: 1800
//! issue: 9093
//! notes: The admitting twin of s28_3_8_3601..3603: the callee only invokes the entry value, here a
//!   closure over the caller's local `k`. 9 per call, 200 calls: 1800.
module Conf;

public class Program
  public static method Peek(ref x: () -> int): int => x();
  public static method Main()
    var n := 0;
    for var i in 0 ..< 200 do
      var k := 9;
      var g: () -> int := lambda() => k;
      n := n + Program.Peek(ref g);
    end for;
    Console.WriteLine(n);
  end Main;
end Program;
