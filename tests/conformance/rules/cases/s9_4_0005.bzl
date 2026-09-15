//! case: s9_4_0005
//! doc: spec
//! section: 9.4
//! title: an external store through a named binding on a settable auto-property remains legal
//! rule: Settable properties keep their purpose: an external store through a
//!   named binding's lvalue chain, such as r.X := 5 on a settable auto-property,
//!   remains legal, the store landing in r's storage, announced by the
//!   assignment itself.
//! expect: run
//! stdout: 5
module Conf;

public record Tally
  public property Hits: int;

  public constructor(hits: int)
    Hits := hits;
  end constructor;
end Tally;

public class Program
  public static method Main()
    var t := new Tally(0);
    t.Hits := 5;
    Console.WriteLine(t.Hits);
  end Main;
end Program;
