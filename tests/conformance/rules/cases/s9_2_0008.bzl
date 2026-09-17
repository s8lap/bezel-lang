//! case: s9_2_0008
//! doc: spec
//! section: 9.2
//! title: an explicit field declaration disqualifies auto-hashable and equality use is rejected
//! rule: A body record is not auto-hashable when it contains an explicit field
//!   declaration (storage that is not an auto-property), unless the field is
//!   marked equality skip; = on such a record without an explicit overload is a
//!   compile error.
//! expect: reject
//! diagnostic: auto-hashable
module Conf;

public record Counter
  private _count: int;

  public constructor(count: int)
    _count := count;
  end constructor;
end Counter;

public class Program
  public static method Compare(a: Counter, b: Counter): bool => a = b;
end Program;
