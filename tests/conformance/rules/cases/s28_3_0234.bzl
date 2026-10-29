//! case: s28_3_0234
//! doc: spec
//! section: 28.3
//! title: A loop rebind of the alias followed by an unconditional rebind before the pin leaves nothing frozen
//! rule: Only a live local borrow of the consumed binding's current value freezes the consume (spec 28.3.9). Here the alias is rebound, or the pin sits in a different branch or after the rebind, so the pin borrows another owner (or none) and the consume of the first owner is admitted; the program runs with each destructor once and each use before its owner's disposal.
//! cites: 28.3, 28.10
//! expect: run
//! output: D:2
//!   D:1
//!   U:3
//!   D:3
//!   D:4
//!   D:2
//!   D:1
//!   U:3
//!   D:3
//!   D:4
//!   END
module Conf;

public sealed class Q
  private _id: int;
  public constructor(id: int)
    _id := id;
  end constructor;
  destructor
    Console.WriteLine($"D:{_id}");
  end destructor;
  public method Use()
    Console.WriteLine($"U:{_id}");
  end Use;
end Q;

public sealed class R
  private _id: int;
  public owned f: Q;
  public constructor(id: int, owned q: Q)
    _id := id;
    f := q;
  end constructor;
  destructor
    Console.WriteLine($"D:{_id}");
  end destructor;
  public method Use()
    Console.WriteLine($"U:{_id}");
  end Use;
  public static method Take(owned r: R)
    using var k := move r;
  end Take;
  public static method TakeQ(owned q: Q)
    using var k := move q;
  end TakeQ;
  public static method Pick<T>(x: T, y: T): T
    return x;
  end Pick;

  public static method Body(k: int)
    var a := new R(2, new Q(1));
    var c := new R(3, new Q(4));
    var b := c;
    for var i in 0..<1 do b := a; end;
    b := c;
    using var p := b;
    R.Take(move a);
    p.Use();
  end Body;
end R;

public class Program
  public static method Main(): int
    for var it in 0..<1 do
      R.Body(0);
      R.Body(1);
    end;
    Console.WriteLine("END");
    return 0;
  end Main;
end Program;
