//! case: s8_5_2301
//! doc: spec
//! section: 8.5
//! title: an inline array of records boxed to Object renders the element
//!   qualified
//! rule: the element recursing in that form, as the class bullet's type
//!   arguments do): `"[2]M.Rec"` for an element type `Rec` declared in
//!   module `M`
//! cites: 2.13, 8.6
//! expect: run
//! output: [2]Conf.Rec
//! issue: 7794
//! notes: Red on base: the pre-fix compiler printed the element unqualified
//!   (`[2]Rec`), the staging tip prints the FullName rendering. Same name
//!   as the direct call, reached through the Object box (8.6).
module Conf;

public record Rec(X: int);

public class Program
  public static method Main()
    var a: [2]Rec := [new Rec(1), new Rec(2)];
    var o: Object := a;
    Console.WriteLine(o.ToString());
  end Main;
end Program;
