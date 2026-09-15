//! case: l4_15_0004
//! doc: stdlib
//! section: 4.15
//! title: an ArraySpan<T> constructed from a named, bound array is admitted
//! rule: Bind the source to a named local whose scope outlives the record,
//!   and pass that -- the remedy for the no-temporary-source rule, applied
//!   to the Array<T>-backed span family.
//! cites: 4.15, 9.7
//! expect: run
//! stdout: 3
module Conf;

public class Program
  public static method Main()
    using var xs: Array<int> := [1, 2, 3];
    var s := new ArraySpan<int>(xs, 0, 3);
    Console.WriteLine(s[2]);
  end Main;
end Program;
