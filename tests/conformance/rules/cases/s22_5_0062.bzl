//! case: s22_5_0062
//! doc: spec
//! section: 22.5
//! title: Path.Combine("x", a := "y") is rejected: a is already bound positionally
//! rule: naming a parameter already bound positionally
//! cites: 22.5, 7.4
//! expect: reject
//! diagnostic: already bound
module Conf;

import System.IO;

public class Program
  public static method Main()
    Console.WriteLine(Path.Combine("x", a := "y"));
  end Main;
end Program;
