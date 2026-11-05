//! case: s28_3_9_0001
//! doc: spec
//! section: 28.3.9
//! title: moving an owned Array local into an owned parameter while an alias is read afterwards is
//!   rejected
//! rule: Consuming an owned **binding** — a local or an `owned` parameter — while a live local borrow of
//!   its current value exists is a compile error
//! cites: 28.3.9
//! expect: reject
//! diagnostic: live
module Conf;

public class Program
  static method SinkS(owned a: Array<string>): int
    return a.Count;
  end SinkS;

  public static method Main()
    var t: string := "a,bc,dc";
    var k := 0;
    var a := t.Split(",");
    var b := a;
    k := k + SinkS(move a);
    k := k + b.Count;

    Console.WriteLine(k);
  end Main;
end Program;
