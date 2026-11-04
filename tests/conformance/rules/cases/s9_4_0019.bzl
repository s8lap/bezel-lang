//! case: s9_4_0019
//! doc: spec
//! section: 9.4
//! title: a record method returns its update with `with` instead of storing into this
//! rule: The diagnostic names the record update idiom: return the updated value,
//!   built with `with` (§9.5) —
//! cites: 9.5
//! expect: run
//! stdout: 5 0
//! notes: twin of s9_4_0011 (one legal edit: the store becomes the `with`
//!   update the diagnostic names); the receiver is left unchanged.
module Conf;

public record Tally
  public property Hits: int;

  public constructor(hits: int)
    Hits := hits;
  end constructor;

  public method Reset(): Tally => this with(Hits := 0);
end Tally;

public class Program
  public static method Main()
    var t := new Tally(5);
    var r := t.Reset();
    Console.WriteLine($"{t.Hits} {r.Hits}");
  end Main;
end Program;
