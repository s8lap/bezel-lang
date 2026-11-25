//! case: s28_11_9862
//! doc: spec
//! section: 28.11
//! title: A generic indexer-set of a function value into an occupied slot displaces the old reference
//! rule: The slot kind alone determines what every element operation does
//! cites: 28.11, 28.9
//! expect: run
//! stdout: 600
//! notes: Harvested from the #9320 review (q03_indexer_displace). The store row for shared-reference
//!   slots covers the indexer-set: `xs[0] := x` stores the reference (a copy slot or a borrow slot
//!   would differ, an owned slot disposes the previous value, which a runtime-managed reference
//!   never needs). Trace per round: [f]; SetTo g gives [g]; SetTo g again gives [g] (the same
//!   closure stored over itself); the read after the block is g() = 3; 200 rounds = 600.
module Conf;
import System.Collections;

public class G
  public static method AddTo<T>(xs: Array<T>, x: T)
    xs.Add(x);
  end AddTo;

  public static method SetTo<T>(xs: Array<T>, x: T)
    xs[0] := x;
  end SetTo;
end G;

public class Program
  public static method Main(): int
    var acc := 0;
    for var i in 0 ..< 200 do
      using var arr: Array<() -> int> := new Array<() -> int>();
      do
        var f: () -> int := lambda() => 2;
        var g: () -> int := lambda() => 3;
        G.AddTo(arr, f);
        G.SetTo(arr, g);
        G.SetTo(arr, g);
      end;
      acc := acc + arr[0]();
    end for;
    Console.WriteLine(acc);
    return 0;
  end Main;
end Program;
