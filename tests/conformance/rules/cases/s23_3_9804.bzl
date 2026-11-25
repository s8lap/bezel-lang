//! case: s23_3_9804
//! doc: spec
//! section: 23.3
//! title: A static and an instance block-bodied function-typed property of the same name in two classes, 200x
//! rule: Each accessor uses `=> expression` or `=> do ... end`.
//! cites: 23.3, 22.12
//! expect: run
//! output: 20600
//! notes: Pins #9224 (pick 08508faec): a read of a block-bodied static property must invoke a receiver-
//!   less getter. A.U is static (no receiver), b.U is an instance property; each read calls its own
//!   getter: 2 + 101 = 103 per iteration, 20600 over 200.
module Conf;

public class A
  public static property U: (int) -> int
    get => do return lambda(x: int) => x + 1; end;
  end U;
end A;

public class B
  public property U: (int) -> int
    get => do return lambda(x: int) => x + 100; end;
  end U;
end B;

public class Program
  public static method Main(): int
    using var b := new B();
    var t := 0;
    var i := 0;
    while i < 200 do
      t := t + A.U(1) + b.U(1);
      i := i + 1;
    end while;
    Console.WriteLine(t);
    return 0;
  end Main;
end Program;
