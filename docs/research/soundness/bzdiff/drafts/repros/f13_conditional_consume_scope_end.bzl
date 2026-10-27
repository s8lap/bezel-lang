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
  public static method Body(flag: bool)
    var b := new R(2, new Q(1));
    if flag then
      R.Take(move b);                 // consumed on one path only: b is maybe-consumed after the join (K7.4)
    end;
  end Body;                           // K7.4: "scope-end disposal per-path"; r35 rejects the drop instead
end R;

public class Program
  public static method Main(args: Array<string>): int
    R.Body(args.Count > 0);
    Console.WriteLine("END");
    return 0;
  end Main;
end Program;
