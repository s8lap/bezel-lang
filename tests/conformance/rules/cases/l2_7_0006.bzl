//! case: l2_7_0006
//! doc: stdlib
//! section: 2.7
//! title: Random seeds given as byte, uint, char or named long produce the stream of the equal long seed
//! rule: two instances constructed with the same seed and driven with the same calls produce the same values
//! cites: 2.7, 5.1
//! expect: run
//! output: True
//!   True
//!   True
//!   True
module Conf;

public class Program
  public static method Main()
    var b: byte := 5;
    var u: uint := 5;
    var c: char := 'a';
    Console.WriteLine(new Random(b).Next() = new Random(5L).Next());
    Console.WriteLine(new Random(u).Next() = new Random(5L).Next());
    Console.WriteLine(new Random(c).Next() = new Random(97L).Next());
    Console.WriteLine(new Random(seed := 5L).Next() = new Random(5L).Next());
  end Main;
end Program;
