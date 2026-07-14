// RUN: %bezelc -c -o %t.o %s
//
// Companion accept test to the nullable-value-type reject (issue #1715),
// spec §32.5, v0.196.
//
// §32.5 "Nullable boundary forms": `CString?`, `WString?`, and `RawBytes?` are
// admitted as possibly-NULL pointers (Bezel `null` marshals to C `NULL`). The
// #1715 nullability guard must NOT over-reject these legitimate nullable
// pointer carriers.

module M;
extern "c"
  public method takes_cstr_opt(s: CString?): int;
  public method takes_wstr_opt(s: WString?): int;
  public method takes_bytes_opt(b: RawBytes?): int;
end;
