//! case: s4_6_0001
//! doc: spec
//! section: 4.6
//! title: string Length returns the number of code points
//! rule: s.Length returns the number of code points in s. O(1).
//! cites: 4.6
//! expect: run
//! stdout: 5
module Conf;

public class Program
  public static method Main()
    Console.WriteLine($"{"hello".Length}");
  end Main;
end Program;
