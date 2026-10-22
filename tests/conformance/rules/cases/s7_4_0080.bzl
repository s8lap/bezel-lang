//! case: s7_4_0080
//! doc: spec
//! section: 7.4
//! title: a readonly field narrowing on this survives a reassignment of an unrelated local
//! rule: "one field off an admitted root" ([§7.4]) is the tracked shape, and "a fact rides its root (reassigning `v` drops it)" ([§7.4]): only a reassignment of the root drops the fact, so reassigning the local `h` leaves `this.F` narrowed.
//! expect: run
//! output: 3
//!   3
//!   2
module Conf;
public record Pt(X: int, Y: int);
public class H
  public readonly F: Pt?;
  public constructor(f: Pt?) this.F := f; end constructor;
  public method Run(): int
    var i := 0; var h := new H(null);
    if F <> null then
      while i < 2 do Console.WriteLine(F.X + this.F.Y); h := new H(null); i += 1; end;
    end;
    return i;
  end Run;
end H;
public class P
  public static method Main(): int
    var h := new H(new Pt(1, 2));
    Console.WriteLine(h.Run());
    return 0;
  end Main;
end P;
