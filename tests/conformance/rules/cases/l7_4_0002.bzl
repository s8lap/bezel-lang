//! case: l7_4_0002
//! doc: stdlib
//! section: 7.4
//! title: Path.GetExtension returns "" when the path has no extension
//! rule: GetExtension(path): string; // "" when none
//! cites: 7.4
//! expect: run
//! stdout: []
module Conf;

import System.IO;

public class Program
  public static method Main()
    Console.WriteLine("[" + Path.GetExtension("archive") + "]");
  end Main;
end Program;
