//! case: l3_3_0006
//! doc: stdlib
//! section: 3.3
//! title: ReadOnlyStringSpan.SequenceEqual takes the array-backed char span
//! rule: public pure method SequenceEqual(other: ReadOnlyArraySpan<char>):
//!   bool; ... `SequenceEqual(other)` returns `true` if the two spans have
//!   the same length and matching code points at every position.
//! cites: 3.3, 4.15
//! expect: run
//! output: True
//!   False
//! issue: 5921
module Conf;

public class Program
  public static method Main()
    using var cs: Array<char> := ['a', 'b'];
    var s := "ab".AsSpan();
    Console.WriteLine(s.SequenceEqual(cs.AsReadOnlySpan()));
    var t := "ax".AsSpan();
    Console.WriteLine(t.SequenceEqual(cs.AsReadOnlySpan()));
  end Main;
end Program;
