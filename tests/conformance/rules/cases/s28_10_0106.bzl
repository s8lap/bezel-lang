//! case: s28_10_0106
//! doc: spec
//! section: 28.10
//! title: handing the holder to an owned parameter under a live pin of a move-only choice field is rejected
//! rule: the frozen kills (a move, a transfer, a reassignment of the binding) are compile errors
//! cites: 28.3, 15.3
//! expect: reject
//! diagnostic: pins its source
module Conf;
public class Res
  public constructor() end constructor;
  destructor end destructor;
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
  static method Take(owned h: H)
    h.Dispose();
  end Take;

  static method Go()
    var h := new H();
    using var x := h.F;
    Take(move h);
  end Go;

  public static method Main()
    Go();
    Console.WriteLine(1);
  end Main;
end Program;
