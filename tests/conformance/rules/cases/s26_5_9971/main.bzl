//! case: s26_5_9971
//! doc: spec
//! section: 26.5
//! title: an imported extension property over the built-in record DateTime reads its receiver
//! rule: the compiler searches the current module and all imported modules
//! cites: 26.5, 26.2, 2.8.2
//! expect: run
//! issue: 9247
//! stdout: 2025
module App;
import Lib;

public class Program
  public static method Main()
    var d := new DateTime(2024, 3, 16);
    Console.WriteLine(d.NextYear);
  end Main;
end Program;
