//! case: l1_9301
//! doc: stdlib
//! section: 1
//! title: FrozenArray declares Length, never Count
//! rule: `Length` measures a fixed or contiguous extent — text (`String`, `StringBuilder`, `ReadOnlyStringSpan`), buffers and spans (§4.14–§4.15), `FrozenArray<T>`
//! cites: 1, 4.19
//! expect: reject
//! diagnostic: count
//! notes: Harvested from the #9185 review (r2). `FrozenArray<T>` is listed among the `Length` types and
//!   declares no `Count`; `f.Count` is a member that does not exist. Accepting twin: `f.Length`
//!   (l4_19_9302).
module Conf;
public class Program
  public static method Main(): int
    using var s := new Array<int>();
    s.Add(7);
    using var f := FrozenArray<int>.From(s);
    var x := f.Count;
    return 0;
  end Main;
end Program;
