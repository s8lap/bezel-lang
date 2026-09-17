//! case: s1_4_0003
//! doc: spec
//! section: 1.4
//! title: a contextual keyword is an ordinary identifier outside its position
//! rule: Contextual keywords have meaning only in specific syntactic
//!   positions; used elsewhere they are ordinary identifiers.
//! cites: 1.4, 3.2
//! expect: run
//! stdout: 5
module Conf;

public class Program
  public static method Main()
    var get := 5;
    Console.WriteLine($"{get}");
  end Main;
end Program;
