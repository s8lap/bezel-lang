//! case: l3_1_0073
//! doc: stdlib
//! section: 3.1
//! title: a StringBuilder takes an initial string or an int capacity at its one-argument position
//! rule: public constructor(initial: string);
//! cites: 3.1, 17.7
//! expect: run
//! stdout: abcd
module Conf;

public class Program
  public static method Main()
    using var a := new StringBuilder("ab");
    using var b := new StringBuilder(16);
    b.Append("cd");
    Console.WriteLine($"{a.ToString()}{b.ToString()}");
  end Main;
end Program;
