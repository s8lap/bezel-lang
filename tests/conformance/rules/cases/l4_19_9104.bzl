//! case: l4_19_9104
//! doc: stdlib
//! section: 4.19
//! title: ToArray of a FrozenArray<string?> is a fresh mutable copy independent of the frozen
//!   original
//! rule: public method ToArray(): owned Array<T>;   // fresh mutable copy back out
//! cites: 4.19, 28.11, 3.6
//! expect: run
//! output: 1780
//! issue: 9121
//! notes: Pins the #9100/#9107/#9121 stack (picks 7c5182e2e..d7a045e3d): the element take/release
//!   of a `string?` slot in a generic collection or FrozenArray. A `string?` slot is a
//!   shared-reference slot like `string` (3.6 Nullable forms; 28.11 element slot policy), so
//!   the store takes one shared reference and the release gives it back; a mis-balanced
//!   reference shows as a crash or a wrong total. The 200-iteration loop is the program's own,
//!   the output is a checksum of the string lengths.
module Conf;

public class Program
  public static method Main(): int
    var tot := 0;
    for var i in 0..<200 do
      var s := new Array<string?>(); s.Add("w" + i.ToString()); s.Add(null);
      using var f := FrozenArray<string?>.From(s);
      using var a := f.ToArray();
      a[0] := "zz"; a.Add("k" + i.ToString());
      for var x in a do tot := tot + (x ?? "").Length; end;
      for var x in f do tot := tot + (x ?? "").Length; end;
    end;
    Console.WriteLine(tot);
    return 0;
  end Main;
end Program;
