//! case: l2_13_2301
//! doc: stdlib
//! section: 2.13
//! title: an inline array with a nullable record element renders the
//!   trailing ?
//! rule: Nullability inside a composite's identity (rules 3–5) is part of
//!   that identity and renders.
//! cites: 8.5, 15.1
//! expect: run
//! output: [2]Conf.Rec?
//! issue: 7794
//! notes: Red on base: the pre-fix compiler printed the element unqualified
//!   (`[2]Rec?`), the staging tip prints the FullName rendering. The
//!   element type is the nullable Rec? (spec 15.1 admits no nullable
//!   inline-array type, so the ? binds the element).
module Conf;

public record Rec(X: int);

public class Program
  public static method Main()
    var n: [2]Rec? := [null, null];
    Console.WriteLine(n.ToString());
  end Main;
end Program;
