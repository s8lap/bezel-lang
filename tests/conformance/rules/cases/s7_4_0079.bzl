//! case: s7_4_0079
//! doc: spec
//! section: 7.4
//! title: a static readonly field narrowing survives a reassignment of a local
//! rule: "participates in flow narrowing" ([§7.4]) for a nullable `readonly` field, `static` included, with the tracked shape `TypeName._s` ([§7.4]). Reassigning the local `h` is not a reassignment of the root `H`, so the static fact survives the loop.
//! expect: run
//! output: 2
//!   2
module Conf;
public record Pt(X: int, Y: int);
public class H
  public static readonly S: string? := "st";
  public readonly F: Pt?;
  public constructor(f: Pt?) this.F := f; end constructor;
end H;
public class P
  public static method Main(): int
    var h := new H(new Pt(1, 2)); var i := 0;
    if H.S <> null then
      while i < 2 do Console.WriteLine(H.S.Length); h := new H(null); i += 1; end;
    end;
    return 0;
  end Main;
end P;
