//! case: s15_4_0015
//! doc: spec
//! section: 15.4
//! title: the empty literal is admitted for a heap-collection target
//! rule: The empty literal `[]` is admitted for every collection target listed
//!   in this section — every collection type has a well-defined empty value —
//!   the target type determining which empty collection it constructs (§15.4).
//! cites: 15.4
//! expect: run
//! stdout: 0
module Conf;

public class Program
  public static method Main()
    using var xs: Array<int> := [];
    Console.WriteLine(xs.Count);
  end Main;
end Program;
