//! case: s26_5_9970
//! doc: spec
//! section: 26.5
//! title: an imported extension block over the built-in class Random applies at a call
//! rule: the compiler searches the current module and all imported modules
//! cites: 26.5, 26.2, 2.8.2
//! expect: run
//! issue: 9247
//! stdout: 7
module App;
import Lib;

public class Program
  public static method Main()
    var r := new Random(1L);
    Console.WriteLine(r.Frob());
  end Main;
end Program;
