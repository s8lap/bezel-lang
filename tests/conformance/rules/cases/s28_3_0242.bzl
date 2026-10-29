//! case: s28_3_0242
//! doc: spec
//! section: 28.3
//! title: A using var pin of an int array element does not freeze a later move of the array
//! rule: A using var over a value read (an int, a record or enum value, a string or a TimeSpan produced from the owner) is a pin of a value that holds no borrow of the owner's storage, so consuming or reassigning the owner afterwards has no live local borrow to freeze (spec 28.3.9: a value-typed read from the owner is no borrow, spec 28.10). The pinned value stays readable and the owner is disposed once.
//! cites: 28.3, 28.10
//! expect: run
//! output: 7
//!   7
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
    var xs := new Array<int>(3, 7);
    using var n := xs[0];
    using var ys := move xs;
    Console.WriteLine($"{n}");
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
