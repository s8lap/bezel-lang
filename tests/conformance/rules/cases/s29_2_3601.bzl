//! case: s29_2_3601
//! doc: spec
//! section: 29.2
//! title: a closure over a frame local yielded from an iterator is rejected at the yield
//! rule: While its source set is non-empty, a function value … may not be returned from the enclosing
//!   method (by `return`, or by `yield return` in an iterator, whose element type announces no source set
//!   either)
//! cites: 29.2, 28.3.8
//! expect: reject
//! diagnostic: yield
//! issue: 9088
//! notes: The issue's repro. The yield is the escape; the later `move a` raises no further question
//!   (s29_2_3602 drops it). Accepting twin s29_2_3603 yields a capture-free lambda.
module Conf;

public class Res
  public N: int := 7;
  public constructor() end constructor;
end Res;

public class Sink
  public static method Take(owned r: Res): int => 1;
end Sink;

public class Program
  public static method Gen(): IEnumerable<() -> int>
    for var i in 0..3 do
      var a := new Res();
      var f := lambda(): int => a.N;
      yield return f;
      var t := Sink.Take(move a);
    end;
  end Gen;
  public static method Main(): int
    var keep: () -> int := lambda(): int => 0;
    for var g in Gen() do
      keep := g;
    end;
    Console.WriteLine($"{keep()}");
    return 0;
  end Main;
end Program;
