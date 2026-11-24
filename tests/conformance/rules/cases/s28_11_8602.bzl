//! case: s28_11_8602
//! doc: spec
//! section: 28.11
//! title: an Array<string?> filled through a call keeps each borrowed string readable after its source ended
//! rule: runtime-managed reference (`string`, function types, `Type`; [§28.9](#sec-runtime-managed-types) — no *declared* element type is a boxed value type, [§9.7](#sec-records-and-disposal), so an `Object` or interface element type is classified by the row below) | shared references | *(rejected at instantiation)*
//! cites: 28.11, 28.9, 3.6
//! expect: run
//! stdout: 690
//! notes: Harvested from the #9064 review (probe u03_array_nullable_borrow_uaf; heap-use-after-free on base per the review). Fill stores the borrowed string parameter s into the caller's Array<string?>; each s goes out of scope at the end of its iteration, but the slot is a shared reference, so the later loop reads every element intact: len("w{i}") = 1+digits(i), sum 200+490 = 690.
module Conf;
import System;
public class Program
  public static method Fill(l: Array<string?>, s: string)
    l.Add(s);
  end Fill;
  public static method Main(): int
    var l := new Array<string?>();
    var tot := 0;
    for var i in 0..<200 do
      var s := "w" + i.ToString();
      Program.Fill(l, s);
    end;
    for var e in l do tot := tot + (e ?? "").Length; end;
    Console.WriteLine(tot);
    return 0;
  end Main;
end Program;
