//! case: s28_10_0101
//! doc: spec
//! section: 28.10
//! title: a marked arm transfer of a using var collection local is rejected: a pin cannot be moved out of
//! rule: a pin cannot be moved out of, only borrowed from
//! cites: 20, 28.3 K3.4
//! expect: reject
//! diagnostic: cannot move out of
module Conf;
public class W
  public static Dropped: int := 0;
  public constructor() end constructor;
  destructor W.Dropped := W.Dropped + 1; end destructor;
end W;
public class Program
  static method Go(flag: bool): int
    using var a := new OwningArray<W>();
    a.Add(new W());
    using var x := (if flag then move a else new OwningArray<W>());
    return x.Count;
  end Go;

  public static method Main()
    Console.WriteLine(Go(true));
  end Main;
end Program;
