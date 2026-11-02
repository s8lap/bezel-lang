//! case: s28_11_0051
//! doc: spec
//! section: 28.11
//! title: move of a slot of a generic owning inline-array field is admitted
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
//! cites: 28.11, 15.1.1, 28.8.1, 14.2
//! expect: accept
module Conf;

public class Res
  public Name: string;
  public constructor(n: string)
    this.Name := n;
  end constructor;
end Res;

public class Box<T> where T: class
  public Slots: [2 owned]T?;

  public method Take(): owned T?
    return move this.Slots[0];
  end Take;
end Box;

public class Program
  public static method Use()
    using var b := new Box<Res>();
    using var t := b.Take();
  end Use;
end Program;
