//! case: s15_4_0002
//! doc: spec
//! section: 15.4
//! title: a sequence literal targeting an inline array type produces a value
//! rule: The target types that admit collection literals include inline
//!   arrays `[N]T`; the literal must have exactly N elements (§15.4).
//! cites: 15.4
//! expect: run
//! stdout: 2
module Conf;

public class Program
  public static method Main()
    var a: [3]int := [1, 2, 3];
    Console.WriteLine(a[1]);
  end Main;
end Program;
