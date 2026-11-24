//! case: s28_11_8601
//! doc: spec
//! section: 28.11
//! title: an Array<string?> holding a freshly built string element returns it intact
//! rule: runtime-managed reference (`string`, function types, `Type`; [§28.9](#sec-runtime-managed-types) — no *declared* element type is a boxed value type, [§9.7](#sec-records-and-disposal), so an `Object` or interface element type is classified by the row below) | shared references | *(rejected at instantiation)*
//! cites: 28.11, 28.9, 3.6
//! expect: run
//! stdout: 890
//! notes: Harvested from the #9064 review (probe i01_issue_literal, the issue's own shape). The element slot of a borrowing-family collection over a runtime-managed element type is a shared reference, so a.Add(s + "q") stores a reference to the concatenation and a[0] reads it back. Each iteration adds len("w{i}q") = 2+digits(i); the sum over 200 iterations is 400+490 = 890. The corpus has no leak oracle, so only the printed value is asserted.
module Conf;
import System;
public class Program
  public static method Main(): int
    var tot := 0;
    for var i in 0..<200 do
      var s := "w" + i.ToString();
      var a := new Array<string?>();
      a.Add(s + "q");
      tot := tot + (a[0] ?? "").Length;
    end;
    Console.WriteLine(tot);
    return 0;
  end Main;
end Program;
