//! case: s5_1_0161
//! doc: spec
//! section: 5.1
//! title: string PadCenter given a nullable char? as its named paddingChar argument is rejected
//! rule: A nullable `T?` is none of these kinds: by the lifting rule above it converts at most to `Object?`, never to `Object` ([§7.1](#sec-non-nullable-and-nullable-types)), so an un-narrowed `Animal?` is rejected at an `Object` parameter as it is at an `Animal` one
//! cites: 5.1
//! expect: reject
//! diagnostic: nullable
module Conf;

public class Program
  public static method Pad(s: string, n: char?): string
    return s.PadCenter(paddingChar := n, totalWidth := 15);
  end Pad;
end Program;
