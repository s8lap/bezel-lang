//! case: s28_8_1_0026
//! doc: spec
//! section: 28.8.1
//! title: a replacement that throws leaves the field unchanged and the cascade disposes it once
//! rule: An exception raised while evaluating the receiver, the index or the
//!   replacement, or by one of those checks ... leaves the location
//!   unchanged
//! cites: 28.8.1, 28.4
//! expect: run
//! output: kept first
//!   dispose first
//!   after
//! issue: 5923
module Conf;

public class Res
  public readonly property Name: string;

  public constructor(name: string)
    Name := name;
  end constructor;

  destructor
    Console.WriteLine($"dispose {Name}");
  end destructor;
end Res;

public class Holder
  private owned _r: Res := new Res("first");

  public method TrySwap()
    try
      using var old := move _r with Holder.Fail();
      Console.WriteLine("swapped");
    catch
      InvalidOperationException => Console.WriteLine($"kept {_r.Name}")
    end;
  end TrySwap;

  private static method Fail(): owned Res
    throw new InvalidOperationException("no replacement");
  end Fail;
end Holder;

public class Program
  public static method Main()
    do
      using var h := new Holder();
      h.TrySwap();
    end;
    Console.WriteLine("after");
  end Main;
end Program;
