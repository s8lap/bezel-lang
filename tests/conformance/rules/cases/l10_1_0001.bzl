//! case: l10_1_0001
//! doc: stdlib
//! section: 10.1
//! title: CString.Length is the byte length excluding the null terminator
//! rule: public readonly property Length: int; // byte length,
//!   excluding the null terminator
//! cites: 10.1
//! expect: run
//! stdout: 3
module Conf;

public class Program
  public static method Main()
    using var cs := CString.From("abc");
    Console.WriteLine(cs.Length);
  end Main;
end Program;
