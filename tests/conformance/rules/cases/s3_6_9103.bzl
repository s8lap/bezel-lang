//! case: s3_6_9103
//! doc: spec
//! section: 3.6
//! title: a static owned readonly FrozenArray<string?> read in a loop keeps its strings
//! rule: a nullable type `V?` is read as `V`, nullability stripped
//! cites: 28.11, 3.6
//! expect: run
//! output: 1000
//! issue: 9121
//! notes: Pins the #9100/#9107/#9121 stack (picks 7c5182e2e..d7a045e3d): the element take/release
//!   of a `string?` slot in a generic collection or FrozenArray. A `string?` slot is a
//!   shared-reference slot like `string` (3.6 Nullable forms; 28.11 element slot policy), so
//!   the store takes one shared reference and the release gives it back; a mis-balanced
//!   reference shows as a crash or a wrong total. The 200-iteration loop is the program's own,
//!   the output is a checksum of the string lengths.
module Conf;

public class Tab
  public static owned readonly Names: FrozenArray<string?> := Tab.Build();
  public static method Build(): owned FrozenArray<string?>
    var s := new Array<string?>(); s.Add("a" + 1.ToString()); s.Add(null); s.Add("bb" + 2.ToString());
    return FrozenArray<string?>.From(s);
  end Build;
end Tab;

public class Program
  public static method Main(): int
    var tot := 0;
    for var i in 0..<200 do
      for var x in Tab.Names do tot := tot + (x ?? "").Length; end;
    end;
    Console.WriteLine(tot);
    return 0;
  end Main;
end Program;
