module Probe;

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
  public static method Body()
    var b := new R(2, new Q(1));
    var c := b;                       // c borrows b
    var a := move c;                  // `move` on a borrow: a spelling error per §28.3.7; accepted
    a.Use();
  end Body;
end R;

public class Program
  public static method Main(): int
    R.Body();
    Console.WriteLine("END");
    return 0;
  end Main;
end Program;
