//! case: s10_4_0006
//! doc: spec
//! section: 10.4
//! title: a distinctly named static method beside the compiler-provided tier is admitted
//! rule: Every enum carries three compiler-provided static members; a user
//!   declaration under a different name does not collide with them.
//! cites: 10.4
//! expect: accept
module Conf;

enum Priority
  Low, Medium, High;

  public static method ParseName(name: string): Priority? => Low;
end Priority;
