//! case: s28_11_9860
//! doc: spec
//! section: 28.11
//! title: Two generic Adds of one function value leave two callable shared-reference slots
//! rule: The slot kind alone determines what every element operation does
//! cites: 28.11, 28.9
//! expect: run
//! stdout: 800
//! notes: Harvested from the #9320 review (r43-rev-m143-9320, q01_twice). A function type is a
//!   runtime-managed reference (28.9), so an `Array<() -> int>` has shared-reference slots (28.11
//!   table): `Add` stores the reference, and the caller's own local keeps its reference. The generic
//!   helper `AddTo<T>` calls `xs.Add(x)` at T := `() -> int`. Two stores of one closure leave two
//!   callable slots (2 + 2 = 4 per round, 200 rounds = 800). Output-only: the pre-fix leak and a
//!   double-release regression are not both visible on stdout, the latter shows as a fault.
module Conf;
import System.Collections;

public class G
  public static method AddTo<T>(xs: Array<T>, x: T)
    xs.Add(x);
  end AddTo;
end G;

public class Program
  public static method Main(): int
    var acc := 0;
    for var i in 0 ..< 200 do
      using var arr: Array<() -> int> := new Array<() -> int>();
      do
        var f: () -> int := lambda() => 2;
        G.AddTo(arr, f);
        G.AddTo(arr, f);
      end;
      acc := acc + arr[0]() + arr[1]();
    end for;
    Console.WriteLine(acc);
    return 0;
  end Main;
end Program;
