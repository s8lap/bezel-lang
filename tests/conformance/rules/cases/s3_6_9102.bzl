//! case: s3_6_9102
//! doc: spec
//! section: 3.6
//! title: FrozenArray<string?> built and read inside async methods keeps its strings
//! rule: a nullable type `V?` is read as `V`, nullability stripped
//! cites: 28.11, 3.6
//! expect: run
//! output: 1090
//! issue: 9121
//! notes: Pins the #9100/#9107/#9121 stack (picks 7c5182e2e..d7a045e3d): the element take/release
//!   of a `string?` slot in a generic collection or FrozenArray. A `string?` slot is a
//!   shared-reference slot like `string` (3.6 Nullable forms; 28.11 element slot policy), so
//!   the store takes one shared reference and the release gives it back; a mis-balanced
//!   reference shows as a crash or a wrong total. The 200-iteration loop is the program's own,
//!   the output is a checksum of the string lengths.
module Conf;

public class G
  public static async method Work(i: int): Task<int>
    var s := new Array<string?>(); s.Add("w" + i.ToString()); s.Add(null);
    using var f := FrozenArray<string?>.From(s);
    var t := 0;
    for var x in f do t := t + (x ?? "").Length; end;
    return t;
  end Work;
  public static async method WorkG<T>(a: T, b: T): Task<int>
    var s := new Array<T>(); s.Add(a); s.Add(b);
    using var f := FrozenArray<T>.From(s);
    return f.Length;
  end WorkG;
end G;

public class Program
  public static async method Main(): Task<int>
    var tot := 0;
    for var i in 0..<200 do
      tot := tot + await G.Work(i);
      tot := tot + await G.WorkG<string?>("q" + i.ToString(), null);
    end;
    Console.WriteLine(tot);
    return 0;
  end Main;
end Program;
