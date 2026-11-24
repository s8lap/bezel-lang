//! case: s28_3_8112
//! doc: spec
//! section: 28.3
//! title: the set-erasing component is found by position when the Object component comes
//!  first
//! rule: At every set-erasing position a function value with a non-empty source set is
//!  rejected, and a capture-free one is admitted
//! cites: 28.3
//! expect: reject
//! diagnostic: capture-free
//! notes: Pins #8962. Same rule as s28_3_8111 with the Object-instantiated component first,
//!  so a check that looked only at the last argument would miss it.
module Conf;

public class Res
  public V: int;
  public constructor() V := 42; end constructor;
end Res;
public record Pair<A, B>(X: A, Y: B);
public class Program
  public static method Main(): int
    var r := new Res();
    var p := new Pair<Object, int>(lambda() => r.V, 1);
    Console.WriteLine(p.Y);
    return 0;
  end Main;
end Program;
