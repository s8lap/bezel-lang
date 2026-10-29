//! case: s5_1_0156
//! doc: spec
//! section: 5.1
//! title: a nullable int? bound to the non-nullable millisecond parameter by name is rejected
//! rule: A nullable `T?` is none of these kinds: by the lifting rule above it converts at most to `Object?`, never to `Object` ([§7.1](#sec-non-nullable-and-nullable-types)), so an un-narrowed `Animal?` is rejected at an `Object` parameter as it is at an `Animal` one
//! cites: 5.1
//! expect: reject
//! diagnostic: nullable
module Conf;

public class Program
  public static method Build(ms: int?): TimeOnly
    return new TimeOnly(1, 2, 3, millisecond := ms);
  end Build;
end Program;
