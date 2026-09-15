//! case: s4_6_0005
//! doc: spec
//! section: 4.6
//! title: a half-open range slice excludes the upper endpoint
//! rule: s[r] ... a half-open s[a..<b] excludes the upper.
//! cites: 4.6
//! expect: run
//! stdout: el
module Conf;

public class Program
  public static method Main()
    Console.WriteLine("hello"[1..<3]);
  end Main;
end Program;
