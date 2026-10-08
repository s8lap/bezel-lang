//! case: s12_4_0006
//! doc: spec
//! section: 12.4
//! title: a user class may derive from Exception, a compiler-known class §31.4 opens
//! rule: A compiler-known class — a class of the standard library, or
//!   `Exception` and the built-in exception types (§31.4) — is a valid base
//!   class only where the text that declares it states that user code may
//!   derive from it: stdlib §1 (*Extensibility*) lists the library classes so
//!   opened, and §31.4 opens the exception family.
//! cites: 12.4, 31.4
//! expect: accept
module Conf;

import System.Threading;

public class E: Exception
end E;
