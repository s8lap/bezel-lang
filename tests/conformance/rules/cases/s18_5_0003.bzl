//! case: s18_5_0003
//! doc: spec
//! section: 18.5
//! title: the P1 format spec renders a percentage with one decimal
//! rule: "`$\"{ratio:P1}\"`            // \"85.0%\""
//! cites: 18.5
//! expect: run
//! stdout: 85.0%
module Conf;

public class Program
  public static method Main()
    var ratio: double := 0.85;
    Console.WriteLine($"{ratio:P1}");
  end Main;
end Program;
