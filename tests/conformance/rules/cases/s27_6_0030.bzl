//! case: s27_6_0030
//! doc: spec
//! section: 27.6
//! title: a method reference to IOwningCollection's Contains is formed over an owning receiver
//! rule: "Method references work for static methods, instance methods (the instance is captured at
//!   formation — by reference for class- or interface-typed receivers, **by copy** for value-shaped
//!   receivers ...)" (§27.6, v0.340). `IOwningCollection<T>` declares `method Contains(item: T): bool`
//!   (stdlib §4.2), and §26.5 rule 1 commits instance resolution to the head's declared member, so
//!   `h.Contains` names that member and forms a bound `(Item) -> bool` reference over the
//!   `IOwningCollection<Item>`-typed `h`. KNOWN DIVERGENCE (#4704): the compiler rejects the
//!   formation in codegen with the #853 fence (".Contains` through a polymorphic collection
//!   interface ... is not yet supported in codegen"), so the reference never forms; no spec sentence
//!   rejects it.
//! cites: 27.6, 26.5, 4.2
//! expect: accept
//! issue: 4704
module Conf;

public class Item
  public method N(): int return 1; end N;
end Item;

public class Demo
  public method Go(): bool
    using var h: IOwningCollection<Item> := new OwningArray<Item>();
    using var it: Item := new Item();
    var f: (Item) -> bool := h.Contains;
    return f(it);
  end Go;
end Demo;
