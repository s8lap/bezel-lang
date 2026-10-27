//! case: l4_1_0049
//! doc: stdlib
//! section: 4.1
//! title: Add is not a member of IReadOnlyList and the call is a compile error
//! rule: `public interface IReadOnlyList<out T>: IReadOnlyCollection<T>` declares
//!   only `readonly property [i: int]: T;` (stdlib §4.1, v0.340) — the indexer, plus the
//!   `Count` property and `GetEnumerator` it inherits. `Add` is declared on
//!   `ICollection<T>` (§4.1), which `IReadOnlyList<T>` does not extend, and no stdlib §5
//!   extension operator is named `Add`; §26.5 rule 1 commits instance resolution to the
//!   head's declared member and rule 2 finds no applicable extension, so the call is a
//!   compile error.
//! cites: 4.1, 26.5
//! expect: reject
//! diagnostic: no member `Add` on `IReadOnlyList`
module Conf;

public class Program
  public static method Main()
    var a: Array<int> := [1, 2, 3];
    var ro: IReadOnlyList<int> := a;
    ro.Add(1);
  end Main;
end Program;
