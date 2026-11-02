//! case: s28_11_0050
//! doc: spec
//! section: 28.11
//! title: move of a slot of an owning inline-array field returned as owned
//! rule: `move coll[i]` is admitted on exactly these receivers, each of whose
//!   indexer names an **owned slot** (the owned column of the element slot
//!   policy above): an `OwningArray<T>` or `OwningDictionary<K, V>` slot; an
//!   `OwningBuffer<T>` slot for class-typed `T` (stdlib
//!   [§4.14](Bezel_Standard_Library.md#sec-owned-buffers-and-spans)); a
//!   `Span<T>` slot for class-typed `T`, a view whose slot is its underlying
//!   buffer's; and a slot of either owning inline-array form, `[N owned]T?`
//!   or `[N owned]T` ([§15.1.1](#sec-owning-inline-arrays)), except through a
//!   `static readonly` field, whose slots are fixed
//!   ([§22.12](#sec-static-members) note (h), [K9.4](#sec-kernel-k9)).
//! cites: 28.11, 15.1.1, 28.8.1
//! expect: run
//! output: a
module Conf;

public class Res
  public Name: string;
  public constructor(n: string)
    this.Name := n;
  end constructor;
end Res;

public class Holder
  public Slots: [2 owned]Res? := [new Res("a"), new Res("b")];

  public method Take(i: int): owned Res?
    return move this.Slots[i];
  end Take;
end Holder;

public class Program
  public static method Main(): int
    using var h := new Holder();
    using var t := h.Take(0);
    Console.WriteLine(t!.Name);
    return 0;
  end Main;
end Program;
