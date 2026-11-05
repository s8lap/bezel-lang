//! case: s28_7_0905
//! doc: spec
//! section: 28.7
//! title: reassigning an owned Array local in an async method while an alias is read afterwards is
//!   rejected
//! rule: Reassigning a plain-`var` owned **local** while a live local borrow of its current value exists
//!   is rejected by the consume-site freeze
//! cites: 28.7, 28.3.9
//! expect: reject
//! diagnostic: live
module Conf;

public class Program
  public static async method Run(t: string): Task<int>
    var a := t.Split(",");
    var b := a;
    a := t.Split(";");
    return b.Count + a.Count;
  end Run;

  public static async method Main(): Task
    var r := await Run("a,bc,dc");
    Console.WriteLine(r);
  end Main;
end Program;
