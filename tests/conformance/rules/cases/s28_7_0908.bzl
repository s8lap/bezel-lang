//! case: s28_7_0908
//! doc: spec
//! section: 28.7
//! title: reassigning an owned Array local on one branch while an alias is read afterwards is rejected
//! rule: Reassigning a plain-`var` owned **local** while a live local borrow of its current value exists
//!   is rejected by the consume-site freeze
//! cites: 28.7, 28.3.9
//! expect: reject
//! diagnostic: live
module Conf;

public class Program
  public static method Main()
    var t: string := "a,bc,dc";
    var k := 0;
    var a := t.Split(",");
    var b := a;
    if k = 0 then
      a := t.Split(";");
    end if;
    k := k + b.Count;

    Console.WriteLine(k);
  end Main;
end Program;
