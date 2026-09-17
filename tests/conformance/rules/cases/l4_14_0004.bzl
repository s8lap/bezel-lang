//! case: l4_14_0004
//! doc: stdlib
//! section: 4.14
//! title: a Span<T> constructed from a named, bound buffer is admitted
//! rule: Bind the source to a named local whose scope outlives the record,
//!   and pass that -- the remedy for the no-temporary-source rule.
//! cites: 4.14, 9.7
//! expect: run
//! stdout: 2
module Conf;

public class Program
  public static method Main()
    using var buf := new OwningBuffer<int>(3, lambda(i: int): int => i);
    var s := new Span<int>(buf, 0, 3);
    Console.WriteLine(s[2]);
  end Main;
end Program;
