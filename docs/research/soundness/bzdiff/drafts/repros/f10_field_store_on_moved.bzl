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
    R.Take(move b);                   // b is moved-from
    b.f := new Q(3);                  // a field store through a moved-from binding: K1.3 error; accepted, SIGSEGV
  end Body;
end R;

public class Program
  public static method Main(): int
    R.Body();
    Console.WriteLine("END");
    return 0;
  end Main;
end Program;
