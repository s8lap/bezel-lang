//! case: s28_3_11_9409
//! doc: spec
//! section: 28.3.11
//! title: an owned chain-call result borrowed inside a no-value async Task method is disposed once
//! rule: Such a temporary is disposed at the **end of the full expression** in which it is produced,
//!   after every use of it within that expression has completed
//! cites: 28.3.11, 30.1
//! expect: run
//! output: 3
//!   3
//!   3
//!   3
module Conf;

public class W
  public static Drops: int := 0;
  public V: int := 3;
  destructor
    W.Drops := W.Drops + 1;
  end destructor;
end W;

public class C
  public static method Make(): () -> owned W
    return lambda(): W => new W();
  end Make;
  public static async method Fire(): Task
    var v := C.Make()().V;
    Console.WriteLine(v);
  end Fire;
  public static async method Main(): Task<int>
    for var j in 0 ..< 3 do
      await C.Fire();
    end for;
    Console.WriteLine(W.Drops);
    return 0;
  end Main;
end C;
