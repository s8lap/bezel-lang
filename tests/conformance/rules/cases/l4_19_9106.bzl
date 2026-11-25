//! case: l4_19_9106
//! doc: stdlib
//! section: 4.19
//! title: a FrozenArray<string?> built from an owned call result reads its strings
//! rule: Copies; the only construction form for T = string.
//! cites: 4.19, 28.11, 3.6
//! expect: run
//! output: 690
//! issue: 9121
//! notes: Pins the #9100/#9107/#9121 stack (picks 7c5182e2e..d7a045e3d): the element take/release
//!   of a `string?` slot in a generic collection or FrozenArray. A `string?` slot is a
//!   shared-reference slot like `string` (3.6 Nullable forms; 28.11 element slot policy), so
//!   the store takes one shared reference and the release gives it back; a mis-balanced
//!   reference shows as a crash or a wrong total. The 200-iteration loop is the program's own,
//!   the output is a checksum of the string lengths. Spec-decided control, green on both arms
//!   (the unfixed compiler prints the same output): a string? slot is a shared-reference slot
//!   (3.6, 28.11), so the totals are the sum of the string lengths.
module Conf;

public class G
  public static method Mk(i: int): owned Array<string?>
    var s := new Array<string?>(); s.Add("w" + i.ToString()); s.Add(null); return s;
  end Mk;
end G;

public class Program
  public static method Main(): int
    var tot := 0;
    for var i in 0..<200 do
      using var f := FrozenArray<string?>.From(G.Mk(i));
      for var x in f do tot := tot + (x ?? "").Length; end;
    end;
    Console.WriteLine(tot);
    return 0;
  end Main;
end Program;
