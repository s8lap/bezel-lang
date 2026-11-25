//! case: s35_5_9810
//! doc: spec
//! section: 35.5
//! title: A payload binding of a nested expression-position owning match may be moved once
//! rule: the warning-free discharges are an owned-consuming call (`RenderOwned(move r)`)
//! cites: 35.5, 28.3, 19.2
//! expect: accept
//! notes: Pins #9237 (pick 8fbe1de84): in an expression-position owning match the payload binding is an
//!   owned local of the arm, so a second consume of it on one path is a use of a moved-from binding.
//!   Twin of s35_5_9809.
module Conf;

public class Res
  destructor
    Console.WriteLine("d");
  end destructor;
end Res;

public class G
  public static method Sink(owned x: Res): int x.Dispose(); return 0; end Sink;
  public static method Peek(x: Res): int return 1; end Peek;
  public static method Peek2(x: Res, y: Res): int return 2; end Peek2;
  public static method Two(owned x: Res, owned y: Res): int x.Dispose(); y.Dispose(); return 2; end Two;
  public static method Use(owned b: Result<Result<Res, string>, string>): int
    var k := 0;
    k := (case move b of Ok(var o) => (case move o of Ok(var v) => G.Sink(move v), Error(var e) => 3), Error(var e) => 2);
    return k;
  end Use;
end G;
