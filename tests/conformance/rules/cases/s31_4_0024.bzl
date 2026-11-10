//! case: s31_4_0024
//! doc: spec
//! section: 31.4
//! title: IFailure is admitted on a class whose user base derives from Exception
//! rule: a class that does not inherit `IFailure` may list it only where no class
//!   in its base chain, other than `Exception` itself, is a built-in or
//!   standard-library exception class; listing it otherwise is a compile error
//!   at the declaration. … A class whose base chain is user-declared down to
//!   `Exception` chooses freely at each level.
//! cites: 31.4
//! expect: accept
//! issue: 8416
module Conf;

public class BadField(message: string): Exception(message)
end BadField;

public class BadPort(message: string): BadField(message), IFailure
end BadPort;
