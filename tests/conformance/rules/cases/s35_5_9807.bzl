//! case: s35_5_9807
//! doc: spec
//! section: 35.5
//! title: A payload binding moved in both operands of a binary expression is rejected
//! rule: the obligation has transferred out; the binding is unreadable until reassigned
//! cites: 35.5, 28.3, 19.2
//! expect: reject
//! diagnostic: already moved
//! notes: Pins #9237 (pick 8fbe1de84): in an expression-position owning match the payload binding is an
//!   owned local of the arm, so a second consume of it on one path is a use of a moved-from binding.
//!   Accepting twin: s35_5_9808.
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
  public static method Use(owned b: Result<Res, string>): int
    var k := 0;
    k := (case move b of Ok(var v) => G.Sink(move v) + G.Sink(move v), Error(var e) => 2);
    return k;
  end Use;
end G;
