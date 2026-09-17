//! case: s32_11_0005
//! doc: spec
//! section: 32.11
//! title: a float in a variadic position is rejected in favor of double
//! rule: Declaring `float`, `bool`, `byte`, `sbyte`, `short`, or `ushort` in a variadic
//!   position is a compile error naming the type to write ("C promotes `float`
//!   variadic arguments to `double`; declare the parameter as `double`").
//! cites: 32.11
//! expect: reject
//! diagnostic: promotes
module Conf;

extern "libexample"
  method SetOptFloat(h: IntPtr, opt: int, value: float): int
      := symbol "example_setopt" variadic(2);
end;
