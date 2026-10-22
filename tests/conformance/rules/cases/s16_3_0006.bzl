//! case: s16_3_0006
//! doc: spec
//! section: 16.3
//! title: a stepped range contains a value its iteration skips
//! rule: Contains(v) is true iff the range is nonempty under iteration (IsEmpty is
//!   false) and min(Start, End) <= v <= max(Start, End), with v = End
//!   additionally excluded when IsEndInclusive is false. Step affects
//!   membership only through emptiness/direction — 0..10 step 2 contains 3.
//! cites: 16.3, 16.4, 17.5
//! expect: run
//! output: 10
//!   12
//!   14
//!   True
//!   True
module Conf;

public class Program
  public static method Main()
    var r := 10..14 step 2;
    for var x in r do
      Console.WriteLine(x);
    end;
    Console.WriteLine(r.Contains(13));
    Console.WriteLine(13 in r);
  end Main;
end Program;
