//! case: l9_4_0002
//! doc: stdlib
//! section: 9.4
//! title: bool.ToString renders exactly "True" or "False"
//! rule: pure method ToString(): string; // "True" or "False"
//! cites: 9.4
//! expect: run
//! stdout: False
module Conf;

public class Program
  public static method Main()
    Console.WriteLine(false.ToString());
  end Main;
end Program;
