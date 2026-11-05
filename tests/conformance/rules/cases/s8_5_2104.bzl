//! case: s8_5_2104
//! doc: spec
//! section: 8.5
//! title: A user class named Lock keeps its own ToString override and a
//!   sibling keeps the default
//! rule: For classes: the type's **qualified name** — the
//!   module-path-qualified rendering stdlib
//! cites: 8.5, 8.3
//! expect: run
//! output: mine
//!   P.Lock2
//!   True
//!   True
//! notes: Green on base and on the staging tip: the control that the stdlib
//!   Lock fix does not capture a user class that happens to be named Lock.
module P;

public class Lock
  public Id: int;

  public constructor(id: int)
    this.Id := id;
  end constructor;

  public override method ToString(): string => "mine";
end Lock;

public class Lock2
  public Id: int;

  public constructor(id: int)
    this.Id := id;
  end constructor;
end Lock2;

public class Program
  public static method Main(): int
    var a := new Lock(1);
    var b := new Lock2(2);
    Console.WriteLine(a.ToString());
    Console.WriteLine(b.ToString());
    Console.WriteLine(a.Equals(a));
    Console.WriteLine(a.GetHashCode() = a.GetHashCode());
    return 0;
  end Main;
end Program;
