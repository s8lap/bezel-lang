//! case: s28_11_0053
//! doc: spec
//! section: 28.11
//! title: move of a slot through a type alias of an owning inline array is admitted
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
//! cites: 28.11, 15.1.1, 28.8.1, 27.8
//! expect: run
//! output: x
module Conf;

public class Res
  public Name: string;
  public constructor(n: string)
    this.Name := n;
  end constructor;
end Res;

type Pool := [2 owned]Res?;

public class Program
  public static method Main(): int
    var a: Pool := [new Res("x"), new Res("y")];
    using var t := move a[0];
    Console.WriteLine(t!.Name);
    return 0;
  end Main;
end Program;
