//! case: l4_16_0003
//! doc: stdlib
//! section: 4.16
//! title: a concrete span passed as IReadOnlySpan<TElement> is boxed call-bounded and reads through it
//! rule: A span passed as an IReadOnlySpan<TElement> parameter is boxed per
//!   the standard rule for borrow-bearing records; the interface parameter
//!   admits any span whose source outlives the call frame.
//! cites: 4.16
//! expect: run
//! stdout: 7
module Conf;

public static class Helpers
  public static method FirstOf(s: IReadOnlySpan<int>): int => s[0];
end Helpers;

public class Program
  public static method Main()
    using var buf := new OwningBuffer<int>(1, lambda(i: int): int => 7);
    Console.WriteLine(Helpers.FirstOf(buf.AsReadOnlySpan()));
  end Main;
end Program;
