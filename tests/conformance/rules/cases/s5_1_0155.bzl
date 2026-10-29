//! case: s5_1_0155
//! doc: spec
//! section: 5.1
//! title: a nullable DateTimeKind? bound to the non-nullable kind parameter by name is rejected
//! rule: A nullable `T?` is none of these kinds: by the lifting rule above it converts at most to `Object?`, never to `Object` ([§7.1](#sec-non-nullable-and-nullable-types)), so an un-narrowed `Animal?` is rejected at an `Object` parameter as it is at an `Animal` one
//! cites: 5.1
//! expect: reject
//! diagnostic: nullable
module Conf;

public class Program
  public static method Build(k: DateTimeKind?): DateTime
    return new DateTime(2020, 1, 1, 1, 1, 1, kind := k);
  end Build;
end Program;
