//! case: s26_5_9706
//! doc: spec
//! section: 26.5
//! title: a declared TextWriter member is admitted on Console.Out and Console.Error through ?.
//! rule: with a `?.` or postfix-`!` receiver read at its underlying non-nullable type
//! cites: 26.5, 7.5, stdlib 7.4
//! expect: run
//! output: after
//! notes: Pins #9223's admitting twin: `Flush` is declared on TextWriter (stdlib 7.4), so the `?.` access
//!   is supplied by rule 1 and admitted; this is the row that catches an over-broad fix that refuses
//!   every `?.` access on a console stream receiver. Flush writes nothing, so the first line of
//!   output is after (the dropped declared `?.` call of #9255 is not exercised: nothing printed by
//!   the access is asserted).
module Conf;

public class Program
  public static method Main()
    Console.Out?.Flush();
    Console.Error?.Flush();
    Console.WriteLine("after");
  end Main;
end Program;
