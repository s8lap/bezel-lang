//! case: s23_3_9802
//! doc: spec
//! section: 23.3
//! title: A block-bodied static function-typed property is read, bound to a local and called, 200x
//! rule: Each accessor uses `=> expression` or `=> do ... end`.
//! cites: 23.3, 22.12
//! expect: run
//! output: 1000
//! notes: Pins #9224 (pick 08508faec): a read of a block-bodied static property must invoke a receiver-
//!   less getter. Box.S is read twice per iteration, once bound to a local f and once called in
//!   place: f(1) is 2 and Box.S(2) is 3, so 5 per iteration and 1000 over 200.
module Conf;

public class Box
  public static property S: (int) -> int
    get => do return lambda(x: int) => x + 1; end;
  end S;
  public static method Main(): int
    var t := 0;
    var i := 0;
    while i < 200 do
      var f := Box.S;
      t := t + f(1) + Box.S(2);
      i := i + 1;
    end while;
    Console.WriteLine(t);
    return 0;
  end Main;
end Box;
