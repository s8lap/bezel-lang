//! case: s22_5_0064
//! doc: spec
//! section: 22.5
//! title: Path.Combine(b := "y") is rejected: required a is left unbound
//! rule: Every parameter that declares no default must be bound
//! cites: 22.5, 7.4
//! expect: reject
//! diagnostic: not supplied
module Conf;

import System.IO;

public class Program
  public static method Main()
    Console.WriteLine(Path.Combine(b := "y"));
  end Main;
end Program;
