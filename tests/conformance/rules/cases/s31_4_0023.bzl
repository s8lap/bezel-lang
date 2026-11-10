//! case: s31_4_0023
//! doc: spec
//! section: 31.4
//! title: IFailure is rejected on a class whose user base derives from ArgumentException
//! rule: a class that does not inherit `IFailure` may list it only where no class
//!   in its base chain, other than `Exception` itself, is a built-in or
//!   standard-library exception class; listing it otherwise is a compile error
//!   at the declaration.
//! cites: 31.4
//! expect: reject
//! diagnostic: base chain
//! issue: 8416
module Conf;

public class BadField(message: string): ArgumentException(message)
end BadField;

public class BadPort(message: string): BadField(message), IFailure
end BadPort;
