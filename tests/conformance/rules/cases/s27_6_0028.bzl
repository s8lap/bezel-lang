//! case: s27_6_0028
//! doc: spec
//! section: 27.6
//! title: a bare GetEnumerator reference on an IOwningEnumerable-typed receiver is rejected
//! rule: "the member resolved on the value exactly as the call `v.Scaled()` is" (§27.6, v0.340). `IOwningEnumerable<T>`
//!   declares `GetOwningEnumerator` and inherits `Dispose` (stdlib §4.13); it has no `GetEnumerator`, which belongs to
//!   `IEnumerable<T>` (stdlib §4.12), so `r.GetEnumerator` names no member at the static type `IOwningEnumerable<Item>`.
//! cites: 26.5, 4.13, 4.12
//! expect: reject
//! diagnostic: getenumerator
module Conf;

public class Item
  public method Get(): int return 4; end Get;
end Item;

public class Program
  public static method Main()
    using var r: IOwningEnumerable<Item> := new OwningArray<Item>();
    var f := r.GetEnumerator;
    Console.WriteLine(1);
  end Main;
end Program;
