//! case: s28_3_0229
//! doc: spec
//! section: 28.3
//! title: an unbound owned temporary passed as a non-owned argument to a borrowing call is admitted
//! rule: "An owned value may also be produced as an **unbound temporary**: the result
//!   of an `owned`-returning call ... that is not bound to any name, not `move`d, and
//!   not returned ..., but used only by *borrowing* operations within the expression —
//!   as the receiver of a further call, **as a non-`owned` argument**, as an operator
//!   operand, as a member or index access ..." Such a temporary is disposed at the end
//!   of the full expression in which it is produced. (§28.3.11, v0.350.)
//! cites: 28.3, 28.3.11
//! expect: run
//! stdout: 200
module Conf;

import System;

public class Program
  public static method MkA(): owned Array<char>
    return [' ', 'a'];
  end MkA;

  public static method Measure(a: Array<char>): int => a.Count;

  public static method Main()
    var n := 0;
    var i := 0;
    while i < 100 do
      n := n + Program.Measure(Program.MkA());
      i := i + 1;
    end while;
    Console.WriteLine(n);
  end Main;
end Program;
