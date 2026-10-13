//! case: l4_16_0005
//! doc: stdlib
//! section: 4.16
//! title: a parameter declared at the retired read-only span interface is rejected
//! rule: Retired. There is no `IReadOnlySpan<TElement>`.
//! cites: 4.16, 4.14, 4.15
//! expect: reject
//! diagnostic: IReadOnlySpan
//! issue: 5921
module Conf;

public class Program
  public static method First(s: IReadOnlySpan<int>): int => s[0];
end Program;
