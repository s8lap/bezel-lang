//! case: s26_5_9981
//! doc: spec
//! section: 26.5
//! title: an extension over a built-in head is a candidate once its module is imported directly
//! rule: they are candidates only from the current module and imported modules
//! cites: 2.8.3, 26.5
//! expect: run
//! stdout: 12
module Top;
import Mid;
import Lib;

public class Program
  public static method Main()
    var r := new Random(1L);
    var v := r.Frob();
    Console.WriteLine(v + Helper.Five());
  end Main;
end Program;
