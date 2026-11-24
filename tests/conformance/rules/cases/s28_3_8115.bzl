//! case: s28_3_8115
//! doc: spec
//! section: 28.3
//! title: a capture-free lambda bound by name to a record component declared Object is
//!  admitted
//! rule: At every set-erasing position a function value with a non-empty source set is
//!  rejected, and a capture-free one is admitted
//! cites: 28.3, 22.5
//! expect: run
//! output: 1
//! notes: Admitting twin of s28_3_8113 and s28_3_8111: the lambda captures nothing, so its
//!  source set is empty and the position admits it.
module Conf;

public class Res
  public V: int;
  public constructor() V := 42; end constructor;
end Res;
public record P(A: int, B: Object);
public class Program
  public static method Main(): int
    var p := new P(1, B := lambda() => 3);
    Console.WriteLine(p.A);
    return 0;
  end Main;
end Program;
