//! case: s28_3_8111
//! doc: spec
//! section: 28.3
//! title: a capturing lambda passed to a record constructor at a component instantiated to
//!  Object is rejected
//! rule: At every set-erasing position a function value with a non-empty source set is
//!  rejected, and a capture-free one is admitted
//! cites: 28.3, 28.1
//! expect: reject
//! diagnostic: capture-free
//! notes: Pins #8962. The generated constructor's parameter for component Y is declared at
//!  the bare type parameter B, instantiated to Object, so it is a set-erasing position; the
//!  lambda captures the class local r, so its source set is non-empty. Twin: s28_3_8115
//!  (capture-free admitted).
module Conf;

public class Res
  public V: int;
  public constructor() V := 42; end constructor;
end Res;
public record Pair<A, B>(X: A, Y: B);
public class Program
  public static method Main(): int
    var r := new Res();
    var p := new Pair<int, Object>(1, lambda() => r.V);
    Console.WriteLine(p.X);
    return 0;
  end Main;
end Program;
