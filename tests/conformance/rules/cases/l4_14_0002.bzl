//! case: l4_14_0002
//! doc: stdlib
//! section: 4.14
//! title: a Span<T> is usable while its OwningBuffer<T> source stays live
//! rule: A borrow-bearing record value is admitted while the source of every
//!   borrow it contains is still live; Span<T> reads through the buffer for
//!   as long as the buffer is not disposed.
//! cites: 4.14, 9.7
//! expect: run
//! stdout: 1
module Conf;

public class Program
  public static method Main()
    using var buf := new OwningBuffer<int>(3, lambda(i: int): int => i);
    var s := buf.AsSpan();
    Console.WriteLine(s[1]);
  end Main;
end Program;
