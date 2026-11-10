//! case: s28_10_0107
//! doc: spec
//! section: 28.10
//! title: a using var pin of a move-only choice field that ended before the holder is moved is admitted
//! rule: the borrow stays live until the using-var's scope ends
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
  static method Go(flag: bool)
    var h := new H();
    if flag then
      using var x := h.F;
    end if;
    using var h2 := move h;
  end Go;

  public static method Main()
    Go(true);
    Console.WriteLine(Res.Dropped);
  end Main;
end Program;
