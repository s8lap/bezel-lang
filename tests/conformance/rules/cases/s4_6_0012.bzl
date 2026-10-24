//! case: s4_6_0012
//! doc: spec
//! section: 4.6
//! title: a range value indexes a string slice
//! rule: `s[r]` for a `Range<int>` value `r` returns the substring spanning `r`.
//!   O(k) for a result of length `k`.
//! cites: 4.6, 9.6
//! expect: run
//! stdout: abc
module Conf;

public class Program
  public static method Main()
    var r: Range<int> := 0..2;
    Console.WriteLine("abcdef"[r]);
  end Main;
end Program;
