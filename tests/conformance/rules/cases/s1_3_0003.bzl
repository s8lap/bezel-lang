//! case: s1_3_0003
//! doc: spec
//! section: 1.3
//! title: string and String name the same type
//! rule: The lowercase keyword `string` and the standard-library type name
//!   `String` name the same type — they are interchangeable in source.
//! expect: run
//! stdout: hi
module Conf;

public class Program
  public static method Main()
    var s: String := "hi";
    var t: string := s;
    Console.WriteLine(t);
  end Main;
end Program;
