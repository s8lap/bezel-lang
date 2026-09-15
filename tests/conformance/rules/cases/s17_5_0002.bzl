//! case: s17_5_0002
//! doc: spec
//! section: 17.5
//! title: in over a string tests character membership via Contains
//! rule: "`var hasChar := 'a' in \"abcdef\";`" — `string` exposes
//!   `Contains(value: char): bool`, reached by `in`'s structural lookup.
//! cites: 17.5
//! expect: run
//! stdout: True
module Conf;

public class Program
  public static method Main()
    var hasChar := 'a' in "abcdef";
    Console.WriteLine(hasChar);
  end Main;
end Program;
