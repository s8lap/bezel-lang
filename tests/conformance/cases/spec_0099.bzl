const H := int.MaxValue / 2;              // 1073741823 : int
const Flags: long := (1 << 10) | (1 << 3); // folds under §6.5 semantics (parens for clarity;
                                           //   shift-with-bitwise is not a §17.2 mandatory mix)
const Greeting := "Hello, " + "world";    // constant string concatenation
const Who := nameof(Greeting);            // "Greeting"
const Cap := (if Flags > 0 then 64 else 32);     // constant conditional
// const B := 2147483647 + 1;             // compile error: constant evaluation overflows (§6.1)
// const F := 1 << 64;                    // compile error: shift count outside 0..31, the int
//                                        //   left operand's domain (§6.5)
// const S := $"v{H}";                    // compile error: interpolated strings are not constant
