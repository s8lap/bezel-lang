//! case: l4_1_0046
//! doc: stdlib
//! section: 4.1
//! title: IsReadOnly reads false through an ICollection-typed receiver
//! rule: "IsReadOnly is `false` for every standard collection implementing
//!   `ICollection<T>` (`Array<T>`, `LinkedList<T>`, `Set<T>`, `Dictionary<K, V>`)" (stdlib §4.1,
//!   v0.340). `ICollection<T>` declares `readonly property IsReadOnly: bool` (§4.1), and §26.5 rule 1
//!   commits instance resolution to the head's declared member, so the `Array<int>` (§4.4) behind an
//!   `ICollection<int>`-typed receiver answers `false` — the value the concrete receiver gives, read
//!   through the interface.
//! cites: 4.1, 4.4, 26.5
//! expect: run
//! stdout: False
module Conf;

public class Program
  public static method Main()
    var a: Array<int> := [1, 2, 3];
    var h: ICollection<int> := a;
    var r := h.IsReadOnly;
    Console.WriteLine(r);
  end Main;
end Program;
