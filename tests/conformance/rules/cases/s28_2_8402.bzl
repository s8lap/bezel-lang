//! case: s28_2_8402
//! doc: spec
//! section: 28.2
//! title: a read of a lambda-body local is admitted when the discarding branch returns before
//!   the join
//! rule: at a join of an explicitly-disposed path with a still-owned path the variable is
//!   **maybe-consumed** and a later use is a compile error
//! cites: 28.2, 17.13
//! expect: run
//! output: 0
//!   1
//! notes: Admitting twin of s28_2_8401 (#9016). `f(true)` takes the discarding branch and
//!   returns 0; `f(false)` skips it and reads the still-owned `x`, returning 1. No path
//!   joins an explicitly-disposed path with a still-owned one.
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
      if c then _ := x; return 0; end if;
      return x.tag;
    end;
    Console.WriteLine(f(true));
    Console.WriteLine(f(false));
    return 0;
  end Main;
end Program;
