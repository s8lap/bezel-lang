//! case: l4_15_0008
//! doc: stdlib
//! section: 4.15
//! title: the byte read tier is not declared on a writable array byte span
//! rule: the §4.14 endianness-aware read tier — every
//!   `Read*{Little,Big}Endian`/`TryRead*` form — is declared again as
//!   `extension ReadOnlyArraySpan<byte>`, member for member and with
//!   identical semantics (`ArraySpan<byte>` reads through `AsReadOnly()`, as
//!   `Span<byte>` does)
//! cites: 4.15, 4.14
//! expect: reject
//! diagnostic: ReadInt16LittleEndian
//! issue: 5921
module Conf;

public class Program
  public static method First(): short
    using var bytes: Array<byte> := [byte(1), byte(2)];
    var s := bytes.AsSpan();
    return s.ReadInt16LittleEndian();
  end First;
end Program;
