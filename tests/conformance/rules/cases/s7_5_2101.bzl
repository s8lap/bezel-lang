//! case: s7_5_2101
//! doc: spec
//! section: 7.5
//! title: ElementAtOrNull evaluates its index argument once per call, null
//!   result included
//! rule: The receiver is evaluated first; if it is null the access does not
//!   occur and nothing to its right in the chain is evaluated
//! cites: 7.5, 5
//! expect: run
//! output: 3
//!   5
//!   -1
//!   3
//! issue: 8232
//! notes: Red on base: the pre-fix compiler rejected ElementAtOrNull on a
//!   concrete collection with 'no member ElementAtOrNull'. It passes on the
//!   staging tip.
module Conf;

public class Counter
  public N: int;

  public constructor()
  end constructor;

  destructor
  end destructor;

  public method Next(): int
    N := N + 1;
    return 0;
  end Next;
end Counter;

public class Program
  public static method Main()
    using var d := new Dictionary<int, int>();
    d.Add(3, 5);
    using var c := new Counter();
    var a := d.ElementAtOrNull(c.Next())?.Key ?? -1;
    var b := d.ElementAtOrNull(c.Next())?.Value ?? -1;
    var x := d.ElementAtOrNull(c.Next() + 9)?.Value ?? -1;
    Console.WriteLine(a);
    Console.WriteLine(b);
    Console.WriteLine(x);
    Console.WriteLine(c.N);
  end Main;
end Program;
