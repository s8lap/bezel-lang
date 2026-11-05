//! case: s28_3_8_0001
//! doc: spec
//! section: 28.3.8
//! title: a closure over an inner-block nullable Array local, stored outward and called after the block,
//!   is
//! rule: whose scope does not strictly enclose any source's scope
//! cites: 28.3.8
//! expect: reject
//! diagnostic: scope
module Conf;

public class Program
  static method CountOf(a: Array<string>?): int
    if a = null then
      return -1;
    end if;
    return a!.Count;
  end CountOf;

  public static method Main()
    var s: string? := "a,bc,dc";
    var k := 0;
    var g: () -> int := lambda() => 0;
    if k = 0 then
      var a := s?.Split(",");
      g := lambda() => CountOf(a);
    end if;
    k := k + g();
    Console.WriteLine(k);
  end Main;
end Program;
