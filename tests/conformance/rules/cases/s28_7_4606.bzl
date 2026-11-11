//! case: s28_7_4606
//! doc: spec
//! section: 28.7
//! title: a use of a field borrow after an explicit Dispose of the owned field through this is rejected
//! rule: or an explicit `r.f.Dispose()` or direct `r.f.DisposeAsync()` where [§28.6](#sec-who-may-dispose) admits it
//! cites: 28.3, 28.6
//! expect: reject
//! diagnostic: explicit disposal
module Conf;

public class Box
  public V: int := 0;
  public constructor() end constructor;
end Box;

public class Holder
  public owned B: Box;
  public constructor()
    B := new Box();
  end constructor;
  public method Run()
    var b := this.B;
    this.B.Dispose();
    Console.WriteLine(b.V);
  end Run;
end Holder;

public class Program
  public static method Main()
    using var h := new Holder();
    h.Run();
  end Main;
end Program;
