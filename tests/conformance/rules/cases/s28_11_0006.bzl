//! case: s28_11_0006
//! doc: spec
//! section: 28.11
//! title: an owning collection over Type is rejected at instantiation
//! rule: In the element-category table, a runtime-managed reference element
//!   type — `string`, a function type, `Type` (§28.9) — is *rejected at
//!   instantiation* in the owning-family column: no holder acquires an
//!   obligation for such a value, so an owned element slot has nothing to own.
//! cites: 28.11, 28.9
//! expect: reject
//! diagnostic: owning
//! issue: 4393
module Conf;

public class Program
  public method Size(): int
    using var types := new OwningArray<Type>();
    return types.Count;
  end Size;
end Program;
