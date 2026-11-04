//! case: s2_7_0041
//! doc: spec
//! section: 2.7
//! title: an alias-qualified name that is no public type of the aliased module is an error (call head)
//! rule: The remainder then resolves against that module's public declarations
//! cites: 2.2, 2.7
//! expect: reject
//! diagnostic: has no public type
module Conf;
import System.IO as sio;

public class Program
  public static method Main()
    sio.NotAType.Exists("x");
    Console.WriteLine("after");
  end Main;
end Program;
