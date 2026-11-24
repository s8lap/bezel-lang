//! case: s28_2_8401
//! doc: spec
//! section: 28.2
//! title: a read of a lambda-body local after a conditional discard of it is rejected
//! rule: at a join of an explicitly-disposed path with a still-owned path the variable is
//!   **maybe-consumed** and a later use is a compile error
//! cites: 28.2, 17.13
//! expect: reject
//! diagnostic: may have already been disposed
//! notes: Pins #9016's flip (a lambda body judges per path like a method body). `_ := x` on one
//!   branch only leaves `x` maybe-consumed at the join, and `x.tag` after it is a later
//!   use. Twin: s28_2_8402, where the discarding branch returns so only the owned path
//!   reaches the read.
module Conf;
import System;
public class Res
  public tag: int;
  public constructor(t: int) this.tag := t; end constructor;
end Res;
public class Program
  public static method Main(): int
    var f := lambda(c: bool): int => do
      var x := new Res(1);
      if c then _ := x; end if;
      return x.tag;
    end;
    Console.WriteLine(f(true));
    return 0;
  end Main;
end Program;
