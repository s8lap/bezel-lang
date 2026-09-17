//! case: l9_6_0001
//! doc: stdlib
//! section: 9.6
//! title: Split keeps every empty segment between separator occurrences
//! rule: Splitting yields every (possibly empty) segment between
//!   separator occurrences: adjacent separators yield an empty element
//!   between them. There is no remove-empty-entries option.
//! cites: 9.6
//! expect: run
//! stdout: 3
module Conf;

public class Program
  public static method Main()
    var parts := "a,,b".Split(',');
    Console.WriteLine(parts.Count);
  end Main;
end Program;
