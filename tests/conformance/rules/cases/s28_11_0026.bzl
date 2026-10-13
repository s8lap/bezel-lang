//! case: s28_11_0026
//! doc: spec
//! section: 28.11
//! title: a generic iterator instantiated at an owning choice is rejected at the call
//! rule: where the argument is or contains a type parameter in scope — a
//!   generic iterator `Once<T>(owned x: T): IEnumerable<T>`, a `class Feed<T>`
//!   holding an `EventSource<T>` — the rejection lands at the instantiations
//!   that substitute a move-only type, through any chain (§3.6 rule 2)
//! cites: 28.11, 15.3, 3.6
//! expect: reject
//! diagnostic: move-only
//! issue: 5819
module Conf;

public class Conn
  public constructor()
  end constructor;
end Conn;

public choice Slot
  Full(owned c: Conn),
  Empty;
end Slot;

public class Source
  public static method Once<T>(x: T): IEnumerable<T>
    yield return x;
  end Once;

  public static method Run(): int
    var n := 0;
    for var s in Source.Once<Slot>(Slot.Empty) do
      n := n + 1;
    end;
    return n;
  end Run;
end Source;
