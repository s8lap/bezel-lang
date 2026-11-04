//! case: s2_7_0042
//! doc: spec
//! section: 2.7
//! title: an alias-qualified name that is a public type of the aliased module resolves (twin)
//! rule: The remainder then resolves against that module's public declarations
//! cites: 2.2, 2.7
//! expect: run
//! stdout: True
module Conf;
import System.IO as sio;

public class Program
  public static method Main()
    var e := sio.File.Exists("x");
    Console.WriteLine(e or true);
  end Main;
end Program;
