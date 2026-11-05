//! case: s28_7_0906
//! doc: spec
//! section: 28.7
//! title: reassigning an owned Array local while an alias is later passed as an argument is rejected
//! rule: Reassigning a plain-`var` owned **local** while a live local borrow of its current value exists
//!   is rejected by the consume-site freeze
//! cites: 28.7, 28.3.9
//! expect: reject
//! diagnostic: live
module Conf;

public class Program
  static method Pass(a: Array<string>): int
    return a.Count;
  end Pass;

  public static method Main()
    var t: string := "a,bc,dc";
    var k := 0;
    var a := t.Split(",");
    var b := a;
    a := t.Split(";");
    k := k + Pass(b);

    Console.WriteLine(k);
  end Main;
end Program;
