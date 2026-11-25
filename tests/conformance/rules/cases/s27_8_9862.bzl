//! case: s27_8_9862
//! doc: spec
//! section: 27.8
//! title: ReferenceEquals through a class alias inside a lambda and a loop answers identity every round
//! rule: see through to the aliased type for identity, equality, method dispatch, and overload resolution
//! cites: 27.8, 8.1, 11.7, 27.3
//! expect: run
//! stdout: 100000
//! issue: 9021
//! notes: Harvested from the #9021 review (q07_alias_in_lambda_and_loop). One fixed instance `a` and a
//!   fresh `b` per round: the lambda compares `a` with `b` (False, no count), and the direct call
//!   compares `b` with itself (True, +1000). 100 rounds = 100000.
module Conf;

public class K end K;
type KA := K;

public class Program
  public static method Main(): int
    var a := new K();
    var f := lambda(x: K, y: K) => KA.ReferenceEquals(x, y);
    var n := 0;
    for var i in 0 ..< 100 do
      var b := new K();
      if f(a, b) then n := n + 1; end;
      if KA.ReferenceEquals(b, b) then n := n + 1000; end;
    end for;
    Console.WriteLine(n);
    return 0;
  end Main;
end Program;
