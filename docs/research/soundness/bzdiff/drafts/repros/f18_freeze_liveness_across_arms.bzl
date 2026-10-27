module Probe;

public sealed class R
  destructor
  end destructor;
  public method Use()
  end Use;
end R;

public class Program
  public static method Main(args: Array<string>): int
    var a := new R();
    var b := a;                       // a borrow of `a`
    if args.Count > 0 then
      a := new R();                   // drop-on-overwrite of `a`; `b` is NOT live here (its only use is on the other arm)
    else
      b.Use();
    end;
    return 0;
  end Main;
end Program;
