//! case: s28_3_8117
//! doc: spec
//! section: 28.3
//! title: a call selecting a declared record constructor is not a set-erasing store of the
//!  lambda argument
//! rule: At every set-erasing position a function value with a non-empty source set is
//!  rejected, and a capture-free one is admitted
//! cites: 28.3
//! expect: run
//! output: 42
//! notes: Pins the boundary of #8962: the call resolves to the declared constructor taking
//!  an int, no function value is stored, so the Object component is not a rejected
//!  position. Rejected counterpart: the generated primary constructor called with a
//!  capturing lambda, as in s28_3_8113.
module Conf;

public class Res
  public V: int;
  public constructor() V := 42; end constructor;
end Res;
public record E(A: int, B: Object)
  public constructor(x: int) this(x, x); end constructor;
end E;
public class Program
  public static method Main(): int
    var r := new Res();
    var d := new E(r.V);
    Console.WriteLine(d.A);
    return 0;
  end Main;
end Program;
