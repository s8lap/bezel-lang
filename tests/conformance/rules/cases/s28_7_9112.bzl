//! case: s28_7_9112
//! doc: spec
//! section: 28.7
//! title: a generic factory result stored at an owned field and overwritten keeps its `string?`
//!   strings
//! rule: **Assignment to an owned field or owned local disposes the displaced value
//! cites: 28.7, 28.11, 14.2
//! expect: run
//! output: 690
//! issue: 9121
//! notes: Pins the #9100/#9107/#9121 stack (picks 7c5182e2e..d7a045e3d): the element take/release
//!   of a `string?` slot in a generic collection or FrozenArray. A `string?` slot is a
//!   shared-reference slot like `string` (3.6 Nullable forms; 28.11 element slot policy), so
//!   the store takes one shared reference and the release gives it back; a mis-balanced
//!   reference shows as a crash or a wrong total. The 200-iteration loop is the program's own,
//!   the output is a checksum of the string lengths.
module Conf;

public class G
  public static method Mk<T>(a: T, b: T): owned FrozenArray<T>
    var s := new Array<T>(); s.Add(a); s.Add(b); return FrozenArray<T>.From(s);
  end Mk;
end G;

public class Box<T>
  public owned items: FrozenArray<T>;
  public constructor(a: T, b: T) items := G.Mk<T>(a, b); end constructor;
  public method Reset(a: T, b: T) items := G.Mk<T>(a, b); end Reset;
end Box;

public class Program
  public static method Main(): int
    var tot := 0;
    for var i in 0..<200 do
      using var b := new Box<string?>("w" + i.ToString(), null);
      b.Reset(null, "q" + i.ToString());
      for var x in b.items do tot := tot + (x ?? "").Length; end;
    end;
    Console.WriteLine(tot);
    return 0;
  end Main;
end Program;
