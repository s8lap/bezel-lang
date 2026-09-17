//! case: s16_3_0003
//! doc: spec
//! section: 16.3
//! title: a byte-typed step silently terminates iteration on overshoot of the endpoint or of the type's range
//! rule: "a range like `0..10 step 256` over `T = byte` constructs without error,
//!   but the enumerator advances by 256 ... and the first computed value past the
//!   start is `256`, which exceeds the upper endpoint, so the iteration yields
//!   only the start value and terminates." ... "Iteration over `200..255 step 100`
//!   for `T = byte` similarly yields `200`; the next computed value (300)
//!   overflows `byte`'s range, so iteration terminates."
//! cites: 16.3
//! expect: run
//! stdout: 1
module Conf;

public class Program
  public static method Main()
    var r1: Range<byte> := 0..10 step 256;
    var count1: int := 0;
    for var i in r1 do
      count1 := count1 + 1;
    end for;
    Console.WriteLine(count1);

    var r2: Range<byte> := 200..255 step 100;
    var count2: int := 0;
    for var i in r2 do
      count2 := count2 + 1;
    end for;
    Console.WriteLine(count2);
  end Main;
end Program;
