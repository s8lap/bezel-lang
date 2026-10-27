//! case: s2_2_0041
//! doc: spec
//! section: 2.2
//! title: a bare reference whose arity no candidate fixes is the arity error
//! rule: "Arity selects among the types rule 1 of §2.7 finds for an identifier: a
//!   reference resolves to the first, in that rule's order, whose arity its type-argument
//!   list fixes; where none has that arity, it resolves to the first of them and is
//!   Appendix C.1's arity error, never an alias, a member or a module prefix" (§2.2,
//!   v0.343). The reference `IAsyncEvent` fixes arity 0. Rule 1 of §2.7 finds `K`'s
//!   inherited nested type `IAsyncEvent<T>` — arity 1 — and the always-available
//!   `IAsyncEvent<out T>` (§2.3's table, arity 1) stands where an imported bare name
//!   does; no candidate has the arity 0 the reference fixes, so the reference is
//!   Appendix C.1's arity error, "a bare name and a miscounted list are both compile
//!   errors naming the declared arity".
//! cites: 2.2, 2.3, 2.7
//! expect: reject
//! diagnostic: generic-argument count
module Conf;

public class B
  public class IAsyncEvent<T>
    public method Get(): int => 1;
  end IAsyncEvent;
end B;

public class K: B
  public method Take(t: IAsyncEvent): int => 0;
end K;
