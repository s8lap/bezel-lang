//! case: l5_3603
//! doc: stdlib
//! section: 5
//! title: SingleOrNull() over an empty source returns null
//! rule: The `*OrNull` variant answers `null` for the missing element only — the band-2 *Expected absence*
//!   its tier is for — so `SingleOrNull()` returns `null` on an empty source and, like `Single()`, raises
//!   `InvalidOperationException` when the source holds more than one element
//! cites: 5, 1
//! expect: run
//! stdout: True
//! issue: 9287
//! notes: Twin of l5_3601: the missing element is the absence the tier answers with null.
module Conf;

public class Program
  public static method Main()
    using var a := new Array<int>();
    Console.WriteLine(a.SingleOrNull() = null);
  end Main;
end Program;
