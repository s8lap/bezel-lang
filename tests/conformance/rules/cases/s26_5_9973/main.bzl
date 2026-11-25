//! case: s26_5_9973
//! doc: spec
//! section: 26.5
//! title: an imported extension block over the built-in class Exception applies at a call
//! rule: the compiler searches the current module and all imported modules
//! cites: 26.5, 26.2, 2.8.2
//! expect: run
//! issue: 9247
//! stdout: tag:hi
module App;
import Lib;

public class Program
  public static method Main()
    using var e := new Exception("hi");
    Console.WriteLine(e.Tag());
  end Main;
end Program;
