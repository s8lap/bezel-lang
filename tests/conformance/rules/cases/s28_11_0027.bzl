//! case: s28_11_0027
//! doc: spec
//! section: 28.11
//! title: the same generic iterator instantiated at a copyable choice runs
//! rule: where the argument is or contains a type parameter in scope — a
//!   generic iterator `Once<T>(owned x: T): IEnumerable<T>` ... — the
//!   rejection lands at the instantiations that substitute a move-only type
//! cites: 28.11, 15.3, 3.6
//! expect: run
//! stdout: 1
module Conf;

public class Conn
  public constructor()
  end constructor;
end Conn;

public choice Slot
  Full(c: int),
  Empty;
end Slot;

public class Source
  public static method Once<T>(x: T): IEnumerable<T>
    yield return x;
  end Once;

  public static method Main()
    var n := 0;
    for var s in Source.Once<Slot>(Slot.Empty) do
      n := n + 1;
    end;
    Console.WriteLine(n);
  end Main;
end Source;
