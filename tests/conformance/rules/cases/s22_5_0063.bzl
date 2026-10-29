//! case: s22_5_0063
//! doc: spec
//! section: 22.5
//! title: Path.Combine("x", "y", c := "z") is rejected: c names no parameter of Combine
//! rule: Each supplied argument binds **exactly one** parameter
//! cites: 22.5, 7.4
//! expect: reject
//! diagnostic: no parameter named
module Conf;

import System.IO;

public class Program
  public static method Main()
    Console.WriteLine(Path.Combine("x", "y", c := "z"));
  end Main;
end Program;
