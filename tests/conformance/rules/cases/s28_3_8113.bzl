//! case: s28_3_8113
//! doc: spec
//! section: 28.3
//! title: a capturing lambda bound by name to a record component declared Object is
//!  rejected
//! rule: At every set-erasing position a function value with a non-empty source set is
//!  rejected, and a capture-free one is admitted
//! cites: 28.3, 22.5
//! expect: reject
//! diagnostic: capture-free
//! notes: Pins #8962. Component B is declared Object, a set-erasing position; the named,
//!  reordered argument binds the capturing lambda to it. Twin: s28_3_8115.
module Conf;

public class Res
  public V: int;
  public constructor() V := 42; end constructor;
end Res;
public record P(A: int, B: Object);
public class Program
  public static method Main(): int
    var r := new Res();
    var p := new P(B := lambda() => r.V, A := 1);
    Console.WriteLine(p.A);
    return 0;
  end Main;
end Program;
