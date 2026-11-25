//! case: l4_19_9105
//! doc: stdlib
//! section: 4.19
//! title: a null-heavy FrozenArray<string?>, an all-null one and an empty one read correctly
//! rule: Copies; the only construction form for T = string.
//! cites: 4.19, 28.11, 3.6
//! expect: run
//! output: 4800
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
      var s := new Array<string?>();
      for var k in 0..<50 do if k mod 7 = 0 then s.Add("w" + k.ToString()); else s.Add(null); end; end;
      using var f := FrozenArray<string?>.From(s);
      var e := new Array<string?>(); e.Add(null); e.Add(null);
      using var g := FrozenArray<string?>.From(e);
      using var z := FrozenArray<string?>.From(new Array<string?>());
      for var x in f do tot := tot + (x ?? "").Length; end;
      tot := tot + g.Length + z.Length;
    end;
    Console.WriteLine(tot);
    return 0;
  end Main;
end Program;
