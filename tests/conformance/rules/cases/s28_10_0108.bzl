//! case: s28_10_0108
//! doc: spec
//! section: 28.10
//! title: an explicit Dispose of the holder is permitted under a live pin of its move-only choice field
//! rule: an explicit `Dispose()` of the source is permitted
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
    var h := new H();
    using var x := h.F;
    h.Dispose();
  end Go;

  public static method Main()
    Go();
    Console.WriteLine(Res.Dropped);
  end Main;
end Program;
