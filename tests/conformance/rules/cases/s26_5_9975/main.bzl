//! case: s26_5_9975
//! doc: spec
//! section: 26.5
//! title: an extension over a built-in head is no candidate when its module is only transitively imported
//! rule: they are candidates only from the current module and imported modules
//! cites: 2.8.3, 26.5
//! expect: reject
//! diagnostic: no member
module Top;
import Mid;

public class Program
  public static method Main()
    var r := new Random(1L);
    var v := r.Frob();
    Console.WriteLine(v + Helper.Five());
  end Main;
end Program;
