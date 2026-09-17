//! case: s6_1_0003
//! doc: spec
//! section: 6.1
//! title: mod follows the dividend's sign
//! rule: mod follows the dividend's sign. var r := -7 mod 3; // -1.
//! cites: 6.1
//! expect: run
//! stdout: -1
module Conf;

public class Program
  public static method Main()
    var r := -7 mod 3;
    Console.WriteLine(r);
  end Main;
end Program;
