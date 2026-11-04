//! case: s35_7_0011
//! doc: spec
//! section: 35.7
//! title: a choice with one admissible and one class payload field is borrow-bearing, so = is a use-site error
//! rule: A choice that is not auto-hashable — any borrow-bearing or owning choice (class-typed payloads are outside the admissible-component rule, exactly as class-typed record fields are)
//! cites: 35.7, 35.4
//! expect: reject
//! diagnostic: auto-hashable
//! issue: 7845
module Conf;

public class K
end K;

public choice Ev
  At(a: DateTime, b: K),
  Never;
end Ev;

public class Program
  public static method Main()
    var k := new K();
    var x := Ev.At(new DateTime(2020, 1, 2, 3, 4, 5), k);
    var y := x;
    Console.WriteLine(x = y);
  end Main;
end Program;
