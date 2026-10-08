//! case: s12_4_0005
//! doc: spec
//! section: 12.4
//! title: a user class may not derive from Mutex, a compiler-known class its text does not open
//! rule: Every other compiler-known class is closed to inheritance exactly as
//!   a `sealed` class is, whether or not its listing writes `sealed`, and a
//!   base-type reference to one in a user declaration is a compile error
//!   naming this rule.
//! cites: 12.4, 30.14
//! expect: reject
//! diagnostic: closed to inheritance
//! issue: 5636
module Conf;

import System.Threading;

public class E: Mutex
end E;
