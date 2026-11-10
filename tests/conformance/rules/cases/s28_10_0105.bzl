//! case: s28_10_0105
//! doc: spec
//! section: 28.10
//! title: a using var pin of a move-only choice field with no consume of the holder is admitted
//! rule: at scope end, no `Dispose()` is called, because the value is not owned by this scope
//! cites: 28.3, 15.3
//! expect: run
//! output: 1
module Conf;
public class Res
  public static Dropped: int := 0;
  public constructor() end constructor;
  destructor Res.Dropped := Res.Dropped + 1; end destructor;
end Res;
public choice OC Text(owned r: Res), Empty; end OC;
public choice VC A(n: int), B; end VC;
public class H
  public owned F: OC := OC.Text(new Res());
  public VF: VC := VC.A(1);
  public constructor() end constructor;
end H;
public class HN
  public owned G: OC? := OC.Text(new Res());
  public constructor() end constructor;
end HN;
public class Program
  static method Go()
    using var h := new H();
    using var x := h.F;
  end Go;

  public static method Main()
    Go();
    Console.WriteLine(Res.Dropped);
  end Main;
end Program;
