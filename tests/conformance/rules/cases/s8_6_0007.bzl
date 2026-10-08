//! case: s8_6_0007
//! doc: spec
//! section: 8.6
//! title: a span passed to Console.WriteLine's Object overload is rejected
//! rule: This is the twin of §28.3's *Set-erasing positions* for a function
//!   value, and rejects the same short list of non-storing calls:
//!   `Console.WriteLine(span)` and `StringBuilder.Append(span)` at their
//!   `Object` parameters, whose spelling is `span.ToString()` or an
//!   interpolation hole
//! cites: 8.6, 9.7
//! expect: reject
//! diagnostic: borrow-erasing
//! issue: 5659
module Conf;

public class Client
  public static method Run()
    using var buf := new OwningBuffer<int>(4);
    var s := buf.AsSpan();
    Console.WriteLine(s);
  end Run;
end Client;
