//! case: s28_3_8_1201
//! doc: spec
//! section: 28.3.8
//! title:a closure over a nullable Array local assigned in a block to a same-scope local is
//!   admitted
//! rule:whose scope does not strictly enclose any source's scope
//! expect: run
//! stdout: 3
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
    var a := s?.Split(",");
    var g: () -> int := lambda() => 0;
    if k = 0 then

      g := lambda() => CountOf(a);
    end if;
    k := k + g();
    Console.WriteLine(k);
  end Main;
end Program;
