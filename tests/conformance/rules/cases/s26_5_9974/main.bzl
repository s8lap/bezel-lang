//! case: s26_5_9974
//! doc: spec
//! section: 26.5
//! title: an imported extension block over the ground built-in head Array<int> applies at a call
//! rule: the compiler searches the current module and all imported modules
//! cites: 26.5, 26.2, 2.8.2
//! expect: run
//! issue: 9247
//! stdout: 12
module App;
import Lib;

public class Program
  public static method Main()
    using var a: Array<int> := [1, 2, 3];
    Console.WriteLine(a.Sum2());
  end Main;
end Program;
