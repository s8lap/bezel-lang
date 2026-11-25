//! case: s28_7_9103
//! doc: spec
//! section: 28.7
//! title: a `h()` local overwritten in a while loop with a `continue` is disposed once per value
//! rule: **Assignment to an owned field or owned local disposes the displaced value
//! cites: 28.7, 28.3
//! expect: run
//! output: sum=0 ds=12
//! issue: 9139
//! notes: Pins #9139 (pick 67908a621): an owned local initialised from a function-value call `h()`
//!   (announced Owned, 28.3 row 1) must be disposed when overwritten; before the fix the first
//!   displaced value leaked. Destructor count `ds` is read after every scope has ended, so the
//!   output is deterministic.
module Conf;

public class W
  public static ds: int := 0;
  public v: int := 3;
  destructor W.ds := W.ds + 1; end destructor;
end W;

public class Sink
  public static method Eat(owned x: W) end Eat;
end Sink;

public class C
  public static method Make(): () -> owned W
    return lambda(): W => new W();
  end Make;
  public static method MakeBoom(): owned W
    throw new Exception("boom");
  end MakeBoom;
  public static method Boom(): int
    throw new Exception("boom");
  end Boom;
end C;

public class App
  public static method Main(): int
    var sum := 0;
    for var i in 0..<4 do
      var h := C.Make();
      var w := h();
      var n := 0;
      while n < 3 do
        n := n + 1;
        if n = 2 then continue; end;
        w := new W();
      end;
    end;
    Console.WriteLine("sum=" + sum.ToString() + " ds=" + W.ds.ToString());
    return 0;
  end Main;
end App;
