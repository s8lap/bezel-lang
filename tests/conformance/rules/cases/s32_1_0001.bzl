//! case: s32_1_0001
//! doc: spec
//! section: 32.1
//! title: a well-formed extern block declaring several methods is admitted
//! rule: An `extern` block opens with `extern` followed by a string-literal library
//!   name, optionally a `convention` clause, and ends with `end;`; each entry
//!   inside is an ordinary method declaration -- visibility, name, parameters,
//!   return type -- followed by `;`, with no body and no per-method `end`.
//! cites: 32.1
//! expect: accept
module Conf;

extern "libc"
  public method puts(s: CString): int;
  public method strlen(s: CString): UIntPtr;
  public method strdup(s: CString): owned CString?;
end;
