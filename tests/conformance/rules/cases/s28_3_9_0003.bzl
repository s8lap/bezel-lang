//! case: s28_3_9_0003
//! doc: spec
//! section: 28.3.9
//! title: reassigning an owned Array local while a closure over it is called afterwards is rejected
//! rule: or through a closure value that is itself still live.
//! cites: 28.3.9
//! expect: reject
//! diagnostic: live
module Conf;

public class Program
  public static method Main()
    var t: string := "a,bc,dc";
    var k := 0;
    var a := t.Split(",");
    var g: () -> int := lambda() => a.Count;
    a := t.Split(";");
    k := k + g();

    Console.WriteLine(k);
  end Main;
end Program;
