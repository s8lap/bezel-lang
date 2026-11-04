//! case: s35_7_0010
//! doc: spec
//! section: 35.7
//! title: a choice with a user-declared class payload is borrow-bearing, so = is a use-site error
//! rule: A choice that is not auto-hashable — any borrow-bearing or owning choice (class-typed payloads are outside the admissible-component rule, exactly as class-typed record fields are)
//! cites: 35.7, 35.4
//! expect: reject
//! diagnostic: auto-hashable
//! issue: 7845
module Conf;

public class Stamp
end Stamp;

public choice Ev
  At(p: Stamp),
  Never;
end Ev;

public class Program
  public static method Main()
    var g := new Stamp();
    var x := Ev.At(g);
    var y := x;
    Console.WriteLine(x = y);
  end Main;
end Program;
