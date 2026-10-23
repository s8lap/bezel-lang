//! case: l4_2_0008
//! doc: stdlib
//! section: 4.2
//! title: Insert is not a member of IOwningCollection and the call is a compile error
//! rule: `public interface IOwningCollection<T>: IReadOnlyCollection<T>, IDisposable`
//!   declares `IsReadOnly`, `Add`, `Clear`, `Contains`, `Remove` and `Dispose` (stdlib
//!   §4.2, v0.340) — `Insert` is declared on `IOwningList<T>` (§4.2) alone, which
//!   `IOwningCollection<T>` does not extend; no stdlib §5 extension operator is named
//!   `Insert`, so §26.5 rule 1 finds no instance member and rule 2 finds no extension:
//!   the call is a compile error.
//! cites: 4.2, 26.5
//! expect: reject
//! diagnostic: no member `Insert` on `IOwningCollection`
module Conf;

public class It
  public method Get(): int => 4;
end It;

public class Program
  public static method Main()
    var oc: IOwningCollection<It> := new OwningArray<It>();
    oc.Insert(0, new It());
  end Main;
end Program;
