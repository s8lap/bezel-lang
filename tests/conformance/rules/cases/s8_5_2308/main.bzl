//! case: s8_5_2308
//! doc: spec
//! section: 8.5
//! title: a positional record component of inline-array type renders the
//!   qualified array name
//! rule: Each component renders as its own `ToString` renders it (virtual
//!   dispatch
//! cites: 9.1, 2.13
//! expect: run
//! output: Holder(a: [2]Lib.Rec)
//! issue: 7794
//! notes: Red on base: the pre-fix compiler printed the element unqualified
//!   (`Holder(a: [2]Rec)`), the staging tip prints the FullName rendering.
//!   Directory case: positional head unqualified (8.5), component rendered
//!   by its own ToString.
module App;

import Lib;

public record Holder(a: [2]Rec);

public class Program
  public static method Main()
    var a: [2]Rec := [new Rec(1), new Rec(2)];
    var h := new Holder(a);
    Console.WriteLine(h.ToString());
  end Main;
end Program;
