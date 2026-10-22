//! case: l3_2_0007
//! doc: stdlib
//! section: 3.2
//! title: Encoding.GetBytes has no overload taking an Array<char>
//! rule: Stdlib §3.2 declares `GetBytes(s: string)`, `GetBytes(s: string, destination: Span<byte>)`,
//!   `GetBytes(s: ReadOnlyStringSpan)` and `GetBytes(s: ReadOnlyStringSpan, destination: Span<byte>)`; none takes an
//!   `Array<char>`, and §5.1 has no implicit conversion from `Array<char>` to `string` or a string span, so the call has
//!   no applicable overload.
//! cites: 3.2
//! expect: reject
//! diagnostic: expects a `string`
module Conf;

public class Program
  public static method Main()
    var a := new Array<char>(3, 'a');
    var d := Encoding.UTF8.GetBytes(a);
    Console.WriteLine(1);
  end Main;
end Program;
