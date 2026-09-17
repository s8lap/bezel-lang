//! case: s4_6_0002
//! doc: spec
//! section: 4.6
//! title: string indexing returns the char at a zero-indexed code-point position
//! rule: s[i] returns the char at code-point position i, zero-indexed. O(1).
//! cites: 4.6
//! expect: run
//! stdout: e
module Conf;

public class Program
  public static method Main()
    Console.WriteLine($"{"hello"[1]}");
  end Main;
end Program;
