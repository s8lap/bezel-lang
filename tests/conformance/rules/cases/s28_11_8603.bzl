//! case: s28_11_8603
//! doc: spec
//! section: 28.11
//! title: an async method returning a LinkedList<string?> of a borrowed string leaves that string intact
//! rule: runtime-managed reference (`string`, function types, `Type`; [§28.9](#sec-runtime-managed-types) — no *declared* element type is a boxed value type, [§9.7](#sec-records-and-disposal), so an `Object` or interface element type is classified by the row below) | shared references | *(rejected at instantiation)*
//! cites: 28.11, 28.9, 3.6
//! expect: run
//! stdout: 890
//! notes: Harvested from the #9064 review (probe t01_async_ll_borrowed). Make adds its borrowed string parameter s to a LinkedList<string?> and returns the list; the task result holds a shared reference to the element, so releasing the list does not release the caller's s. Per iteration t.Result.Count = 1 and s.Length = 1+digits(i): 2+d; sum 400+490 = 890.
module Conf;
import System;
public class C
  public static async method Make(s: string): Task<LinkedList<string?>>
    var r := new LinkedList<string?>();
    r.Add(s);
    return r;
  end Make;
  public static method Main(): int
    var tot := 0;
    for var i in 0..<200 do
      var s := "w" + i.ToString();
      using var t := C.Make(s);
      t.Wait();
      tot := tot + t.Result.Count + s.Length;
    end;
    Console.WriteLine(tot);
    return 0;
  end Main;
end C;
