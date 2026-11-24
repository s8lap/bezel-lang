//! case: s28_3_5_8511
//! doc: spec
//! section: 28.3.5
//! title: a chained call's borrowed result from static storage is never disposed by the reading
//!   statement
//! rule: The result is never owned.
//! cites: 28.3.5, 28.3.11
//! expect: run
//! stdout: 1000 0 0
//! notes: Pins #9040. The chain result is a borrow of the static field's referent, which no
//!   statement owns: no destructor runs during the 200 reads (`bad` sums the destructor
//!   count after each) and none has run when the loop ends. t = 200 * 5 = 1000.
module Conf;
public class K
  public static D: int := 0;
  public readonly N: int;
  public constructor(n: int) N := n; end constructor;
  destructor K.D := K.D + 1; end destructor;
end K;

public class Program
  public static readonly _shared: K := new K(5);
  public static method Use(): () -> K
    return lambda() => Program._shared;
  end Use;
  public static method Main(): int
    var t := 0;
    var bad := 0;
    for var j in 0 ..< 200 do
      t := t + Program.Use()().N;
      bad := bad + K.D;
    end;
    Console.WriteLine($"{t} {bad} {K.D}");
    return 0;
  end Main;
end Program;
