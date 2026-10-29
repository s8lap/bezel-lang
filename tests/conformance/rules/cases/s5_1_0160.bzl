//! case: s5_1_0160
//! doc: spec
//! section: 5.1
//! title: string Insert given a nullable string? as its named value argument is rejected
//! rule: A nullable `T?` is none of these kinds: by the lifting rule above it converts at most to `Object?`, never to `Object` ([§7.1](#sec-non-nullable-and-nullable-types)), so an un-narrowed `Animal?` is rejected at an `Object` parameter as it is at an `Animal` one
//! cites: 5.1
//! expect: reject
//! diagnostic: nullable
module Conf;

public class Program
  public static method Put(s: string, n: string?): string
    return s.Insert(value := n, startIndex := 3);
  end Put;
end Program;
