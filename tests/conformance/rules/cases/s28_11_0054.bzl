//! case: s28_11_0054
//! doc: spec
//! section: 28.11
//! title: move of an owning inline-array slot after an await in an async method is admitted
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
//! cites: 28.11, 15.1.1, 28.8.1, 30.1
//! expect: accept
module Conf;

public class Res
  public Name: string;
  public constructor(n: string)
    this.Name := n;
  end constructor;
end Res;

public class Program
  public static async method Go(): Task<int>
    var a: [2 owned]Res? := [new Res("x"), new Res("y")];
    await Task.Delay(1);
    using var t := move a[1];
    Console.WriteLine(t!.Name);
    return 1;
  end Go;
end Program;
