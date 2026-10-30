//! case: s28_10_0074
//! doc: spec
//! section: 28.10
//! title: a using var owning-choice local cannot be moved out of by a local binding through !
//! rule: it cannot be moved out of, only borrowed from
//! cites: 28.3.7, 17.8, 15.3, 35.3
//! expect: reject
//! diagnostic: moved out of
module Conf;

public class Res
  public constructor() end constructor;
  destructor end destructor;
end Res;

public choice OC
  Text(owned r: Res),
  Empty;
end OC;

public class Program
  public static method T()
    using var o: OC? := OC.Text(new Res());
    var x := o!;
  end T;

  public static method Main(): int
    T();
    return 0;
  end Main;
end Program;
