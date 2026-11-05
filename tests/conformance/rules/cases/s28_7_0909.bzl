//! case: s28_7_0909
//! doc: spec
//! section: 28.7
//! title: reassigning an owned Array local while an alias made by an extension method returning this is
//!   read
//! rule: Reassigning a plain-`var` owned **local** while a live local borrow of its current value exists
//!   is rejected by the consume-site freeze
//! cites: 28.7, 28.3.9
//! expect: reject
//! diagnostic: live
module Conf;

public extension Array<string>
  public method Id(): Array<string> => this;
end extension;

public class Program
  public static method Main()
    var t: string := "a,bc,dc";
    var k := 0;
    var a := t.Split(",");
    var d := a.Id();
    a := t.Split(";");
    k := k + d.Count;

    Console.WriteLine(k);
  end Main;
end Program;
