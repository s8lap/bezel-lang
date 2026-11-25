//! case: s28_7_9111
//! doc: spec
//! section: 28.7
//! title: an owned FrozenArray<string?> field overwritten four times by From keeps the last copy's
//!   strings
//! rule: **Assignment to an owned field or owned local disposes the displaced value
//! cites: 28.7, 28.11, 4.19
//! expect: run
//! output: 1090
//! issue: 9100
//! notes: Pins the #9100/#9107/#9121 stack (picks 7c5182e2e..d7a045e3d): the element take/release
//!   of a `string?` slot in a generic collection or FrozenArray. A `string?` slot is a
//!   shared-reference slot like `string` (3.6 Nullable forms; 28.11 element slot policy), so
//!   the store takes one shared reference and the release gives it back; a mis-balanced
//!   reference shows as a crash or a wrong total. The 200-iteration loop is the program's own,
//!   the output is a checksum of the string lengths.
module Conf;

public class CBox
  public owned items: FrozenArray<string?>;
  public constructor() items := FrozenArray<string?>.From(new Array<string?>()); end constructor;
  public method Reset(src: Array<string?>) items := FrozenArray<string?>.From(src); end Reset;
end CBox;

public class Program
  public static method Main(): int
    var tot := 0;
    for var i in 0..<200 do
      using var b := new CBox();
      for var k in 0..<4 do
        var s := new Array<string?>(); s.Add("w" + k.ToString()); s.Add(null); s.Add("y" + i.ToString());
        b.Reset(s);
        s.Clear();
      end;
      for var x in b.items do tot := tot + (x ?? "").Length; end;
    end;
    Console.WriteLine(tot);
    return 0;
  end Main;
end Program;
