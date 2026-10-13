//! case: s11_2_0111
//! doc: spec
//! section: 11.2
//! title: an indexer whose substituted element type is not matched by the implementation is rejected
//! rule: A read-write member satisfies a read-only interface member of the same name and type: if IReadOnlyList<T> declares `readonly property [i: int]: T` and IList<T> declares `property [i: int]: T`, a single read-write indexer satisfies both.
//!   The implementation's indexer must therefore have the same type as the interface's indexer, which under I<int> is the substituted element type.
//!   Under I<int> the element type is int; the implementation's indexer returns string, which is not exactly int.
//! issue: 6129
//! cites: 11.2
//! expect: reject
//! diagnostic: does not match
module Conf;

public interface I<T>
  readonly property [i: int]: T;
end I;

public class K: I<int>
  public constructor() end constructor;

  public property [i: int]: string => "x";
end K;
