//! case: s35_7_0008
//! doc: spec
//! section: 35.7
//! title: a generic choice with a type parameter named DateTime at a class is borrow-bearing, so = errs
//! rule: A choice that is not auto-hashable — any borrow-bearing or owning choice (class-typed payloads are outside the admissible-component rule, exactly as class-typed record fields are)
//! cites: 35.7, 35.4, 2.3
//! expect: reject
//! diagnostic: auto-hashable
//! issue: 7845
module Conf;

public class K
end K;

public choice C<DateTime>
  At(p: DateTime),
  Never;
end C;

public class Program
  public static method Main()
    var k := new K();
    var x := C<K>.At(k);
    var y := x;
    Console.WriteLine(x = y);
  end Main;
end Program;
