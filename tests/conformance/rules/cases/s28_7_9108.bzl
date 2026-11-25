//! case: s28_7_9108
//! doc: spec
//! section: 28.7
//! title: a generic helper over `() -> owned T` that overwrites a `h()` local disposes the
//!   displaced value
//! rule: **Assignment to an owned field or owned local disposes the displaced value
//! cites: 28.7, 28.3
//! expect: run
//! output: sum=0 ds=8
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

public class G
  public static method Run<T>(h: () -> owned T): owned T where T: class
    var w := h();
    w := h();
    return w;
  end Run;
end G;

public class App
  public static method Main(): int
    var sum := 0;
    for var i in 0..<4 do
      var h := C.Make();
      using var r := G.Run<W>(h);
    end;
    Console.WriteLine("sum=" + sum.ToString() + " ds=" + W.ds.ToString());
    return 0;
  end Main;
end App;
