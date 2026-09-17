//! case: s18_4_0001
//! doc: spec
//! section: 18.4
//! title: a non-verbatim string literal processes its escape sequences
//! rule: "\"Line 1\\nLine 2\"      // escapes processed"
//! cites: 18.4, 18.8
//! expect: run
//! stdout: Line 1
module Conf;

public class Program
  public static method Main()
    Console.WriteLine("Line 1\nLine 2");
  end Main;
end Program;
