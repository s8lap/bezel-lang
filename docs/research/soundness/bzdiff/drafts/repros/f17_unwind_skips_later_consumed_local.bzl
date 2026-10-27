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

public sealed class R: Exception
  private _id: int;
  public owned f: Q;
  public constructor(id: int, owned q: Q)
    base("R");
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
  public static method Body(flag: bool)
    var a := new R(2, new Q(1));
    if flag then
      throw new R(4, new Q(3));       // the exception edge must dispose `a` here (K3.15); it does not
    end;
    var b := move a;                  // a LATER binding transfer of `a` switches the unwind disposal above off (an owned-argument `Take(move a)` does not)
  end Body;
end R;

public class Program
  public static method Main(args: Array<string>): int
    try
      R.Body(args.Count > 0);
    catch
      var e: R => Console.WriteLine("caught"),
    end;
    Console.WriteLine("END");
    return 0;
  end Main;
end Program;
