//! case: s28_10_0109
//! doc: spec
//! section: 28.10
//! title: a using var over a value-typed choice field is not a borrow pin: the holder may be moved
//! rule: which admits `using var` for any value-type or runtime-managed RHS
//! cites: 28.3, 35.4
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
    using var x := h.VF;
    using var h2 := move h;
  end Go;

  public static method Main()
    Go();
    Console.WriteLine(Res.Dropped);
  end Main;
end Program;
