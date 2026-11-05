//! case: l4_1_2201
//! doc: stdlib
//! section: 4.1
//! title: a Stack of strings reports Count 1 after one Push per fresh stack
//! rule: pure readonly property Count: int;
//! cites: 4.11
//! expect: run
//! stdout: 200
//! notes: Trace: each of 200 fresh stacks receives one Push, so Count
//!   (IReadOnlyCollection) is 1 and the sum is 200. Green on the
//!   pre-fix compiler and the tip: a control for the Stack of string
//!   element path.
module Conf;

public class Program
  public static method Main()
    var tot := 0;
    var i := 0;
    while i < 200 do
      var c := new Stack<string>();
      c.Push("a" + (40 + i mod 10).ToString());
      tot := tot + c.Count;
      i := i + 1;
    end;
    Console.WriteLine(tot);
  end Main;
end Program;
