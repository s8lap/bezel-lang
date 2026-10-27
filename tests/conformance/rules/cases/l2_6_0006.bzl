//! case: l2_6_0006
//! doc: stdlib
//! section: 2.6
//! title: Guid.Parse reads the D rendering of the all-zero UUID
//! rule: stdlib §2.6 declares `public static method Parse(s: string): Guid;`
//!     with the comment "accepts the "D" and "N" renderings (§2.18);
//!     FormatException otherwise", and §2.18's `Guid` row reads "`D`
//!     (hyphenated, lowercase), `N` (no hyphens) | no-format = `D`". The
//!     all-zero D rendering parses, and the value it yields is `Guid.Empty` —
//!     "the all-zero UUID" (§2.6).
//! cites: 2.6, 2.18
//! expect: run
//! stdout: True
module Conf;

public class Program
  public static method Main()
    var g := Guid.Parse("00000000-0000-0000-0000-000000000000");
    Console.WriteLine(g = Guid.Empty);
  end Main;
end Program;
