//! case: l4_15_0007
//! doc: stdlib
//! section: 4.15
//! title: a writable array byte span reads the read tier through AsReadOnly
//! rule: the §4.14 endianness-aware read tier — every
//!   `Read*{Little,Big}Endian`/`TryRead*` form — is declared again as
//!   `extension ReadOnlyArraySpan<byte>`, member for member and with
//!   identical semantics (`ArraySpan<byte>` reads through `AsReadOnly()`, as
//!   `Span<byte>` does)
//! cites: 4.15, 4.14
//! expect: run
//! stdout: 513
//! issue: 5921
module Conf;

public class Program
  public static method Main()
    using var bytes: Array<byte> := [byte(1), byte(2)];
    var s := bytes.AsSpan();
    Console.WriteLine(s.AsReadOnly().ReadInt16LittleEndian());
  end Main;
end Program;
