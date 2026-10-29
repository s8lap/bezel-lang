//! case: s22_5_0066
//! doc: spec
//! section: 22.5
//! title: Path.GetFullPath(x := "d/f.txt") is rejected: x is not the parameter name path
//! rule: Each supplied argument binds **exactly one** parameter
//! cites: 22.5, 7.4
//! expect: reject
//! diagnostic: no parameter named
module Conf;

import System.IO;

public class Program
  public static method Main()
    Console.WriteLine(Path.GetFullPath(x := "d/f.txt"));
  end Main;
end Program;
