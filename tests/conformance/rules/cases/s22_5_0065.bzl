//! case: s22_5_0065
//! doc: spec
//! section: 22.5
//! title: Path.GetExtension(p := "d/f.txt") is rejected: p is not the parameter name path
//! rule: Each supplied argument binds **exactly one** parameter
//! cites: 22.5, 7.4
//! expect: reject
//! diagnostic: no parameter named
module Conf;

import System.IO;

public class Program
  public static method Main()
    Console.WriteLine(Path.GetExtension(p := "d/f.txt"));
  end Main;
end Program;
