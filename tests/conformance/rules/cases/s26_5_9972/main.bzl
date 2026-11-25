//! case: s26_5_9972
//! doc: spec
//! section: 26.5
//! title: an imported extension block over the built-in class StringBuilder applies at a call
//! rule: the compiler searches the current module and all imported modules
//! cites: 26.5, 26.2, 2.8.2
//! expect: run
//! issue: 9247
//! stdout: 103
module App;
import Lib;

public class Program
  public static method Main()
    using var sb := new StringBuilder();
    sb.Append("abc");
    Console.WriteLine(sb.Mark());
  end Main;
end Program;
