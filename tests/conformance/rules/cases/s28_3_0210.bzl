//! case: s28_3_0210
//! doc: spec
//! section: 28.3
//! title: the owner of a collection literal reads its own elements
//! rule: "A collection literal (`[1, 2, 3]`) targeting a reference type (`Array<T>`,
//!   `OwningArray<T>`, `Set<T>`, `Dictionary<K, V>`) is **Owned**" (§28.3's
//!   producers table, v0.340), so `a` owns the literal and reads its own elements
//!   (three of them); the owner's scope-end disposal disposes it exactly once.
//! cites: 28.3
//! expect: run
//! stdout: 3
module Conf;

import System;

public class Program
  public static method Main()
    var a: Array<int> := [1, 2, 3];
    Console.WriteLine(a.Count);
  end Main;
end Program;
