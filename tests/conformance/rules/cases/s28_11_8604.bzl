//! case: s28_11_8604
//! doc: spec
//! section: 28.11
//! title: an async method returning a LinkedList<string?> built from a borrowed string, one iteration
//! rule: runtime-managed reference (`string`, function types, `Type`; [§28.9](#sec-runtime-managed-types) — no *declared* element type is a boxed value type, [§9.7](#sec-records-and-disposal), so an `Object` or interface element type is classified by the row below) | shared references | *(rejected at instantiation)*
//! cites: 28.11, 28.9, 3.6
//! expect: run
//! stdout: 3
//! notes: Harvested from the #9064 review (probe g3_async_borrowed_LinkedList_1iter, the single-iteration form of s28_11_8603, kept because the review measured the two-release failure at one iteration). Count = 1 and s.Length = len("w0") = 2, so 3.
module Conf;
import System;
public class Program
  public static async method Make(s: string): Task<LinkedList<string?>>
    var r := new LinkedList<string?>();
    r.Add(s);
    return r;
  end Make;
  public static method Main(): int
    var tot := 0;
    for var i in 0..<1 do
      var s := "w" + i.ToString();
      using var t := Program.Make(s);
      t.Wait();
      tot := tot + t.Result.Count + s.Length;
    end;
    Console.WriteLine(tot);
    return 0;
  end Main;
end Program;
