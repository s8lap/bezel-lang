| # | sev | HEAD status | sub-family | lane | B0 leverage | evidence |
|---|---|---|---|---|---|---|
| #1701 | other | RE-LABEL (rule) | X-spec | re-label | - | constraint-vs-direct TryParse surface split; not a call admission defect |
| #1765 | soundness | RE-LABEL (rule) | X-13 | re-label | - | Ptr<T>.Load/Store is a codegen stub, not a call admission defect |
| #1792 | other | FIXED (MEASURED) | B1 | B1 | - | STALE PREMISE (MEASURED): char.Parse("a") prints a; char.TryParse("a", out c) prints True/a; char.ParseOrNull("a")->not null, ("zz")->null; bool.ParseOrNull("true")->not null, ("nope")->null |
| #2094 | other | RE-LABEL (rule) | X-lot | re-label | - | Counter<K> implementation deferred; needs intrinsic class machinery |
| #2221 | soundness | RE-LABEL (MEASURED) | X-4 | re-label | - | Array<T>.Sort on class elements is an ownership UAF, not admission |
| #2253 | other | REPRODUCES (rule) | B2 | B2a/B2b | N | decimal ToString("Fn") vs "Pn"/"En" rounding disagree (stdlib §2.18) |
| #2256 | other | SPEC-Q (rule) | X-spec | re-label | - | §22.15 pure-construction list omits choice construction |
| #2313 | crash | RE-LABEL (MEASURED) | X-lot | re-label | - | IReadOnlySpan<char> parameter lowers to malformed IR; not a builtin call |
| #2620 | soundness | RE-LABEL (MEASURED) | X-5 | re-label | - | narrowed T? into TaskCompletionSource.TrySetResult: §7.4 unwrap, campaign 5 |
| #2625 | soundness | RE-LABEL (rule) | X-5 | re-label | - | narrowed-nullable not unwrapped at builtin value-type-param sites; campaign 5 |
| #2642 | crash | RE-LABEL (MEASURED) | X-5 | re-label | - | LINQ Contains ICEs on a narrowed T?; stdlib §5, campaign 5 |
| #2647 | soundness | RE-LABEL (rule) | X-8 | re-label | - | SortedSet<T> literal takes the array path; campaign 8 (collection elements) |
| #2651 | other | RE-LABEL (rule) | X-4 | re-label | Y | Dictionary entry-form Add is classified as the K slot (stdlib §4.8) |
| #2662 | other | RE-LABEL (MEASURED) | X-4 | re-label | Y | span.Slice(Range<int>) accepted but undeclared (stdlib §4.14) |
| #2729 | other | RE-LABEL (rule) | X-4 | re-label | - | sorted-dictionary comparer field (stdlib §4.9) |
| #2730 | soundness | RE-LABEL (rule) | X-4 | re-label | Y | OwningArray.AddRange drain; comparer arg silently dropped (stdlib §4) |
| #3396 | other | REPRODUCES (rule) | B0-dx | re-label | N | Object -> user IEquatable<Object> rejected with a stdlib-specific message |
| #3509 | other | RE-LABEL (rule) | X-4 | re-label | - | SortedDictionary reference key is O(n); performance |
| #3826 | other | RE-LABEL (rule) | X-lot | re-label | - | Reverse() semantics vs stdlib §5 |
| #3896 | other | RE-LABEL (rule) | X-4 | re-label | - | collections gated by the closed #2457 Equals/GetHashCode placeholder |
| #3943 | other | RE-LABEL (MEASURED) | X-13 | re-label | - | record arm's non-struct guard does not consult viaNullCondWrapper: a double diagnostic |
| #4120 | soundness | REPRODUCES (MEASURED) | B2 | B2a/B2b | N | MEASURED: CString.ToString() prints "System.CString"; WString/RawBytes likewise |
| #4126 | soundness | REPRODUCES (rule) | B2 | B2a/B2b | N | CancellationToken/Range/Guid GetHashCode wrong; Stopwatch.ToString SIGSEGV |
| #4255 | crash | RE-LABEL (rule) | X-lot | re-label | - | bezelc OOM on a 2000-deep record chain; not admission |
| #4481 | soundness | SPEC-Q (rule) | X-spec | re-label | - | char classification is ASCII-only above the #4420 guard |
| #4585 | soundness | RE-LABEL (MEASURED) | X-5 | re-label | - | Array<Guid?> stores a corrupted payload; campaign 5 |
| #4790 | soundness | RE-LABEL (rule) | X-13 | re-label | Y | equal-arity ctor binds by name but mismatches type (a USER ctor) |
| #4797 | other | RE-LABEL (MEASURED) | X-lot | re-label | - | coll.Drain() as a call argument does not lower |
| #4805 | crash | RE-LABEL (rule) | X-13 | re-label | - | combined-form record ctor with a ref parameter ICEs |
| #4890 | other | SPEC-Q (rule) | X-spec | re-label | - | NaN/Infinity spelling is silent in the spec |
| #5355 | other | RE-LABEL (rule) | X-1 | re-label | - | builtinHead unwind for imported Channel owned params; campaign 1 |
| #5409 | soundness | RE-LABEL (rule) | B0-enabler | re-label | - | retire the name-keyed shadowing workarounds after #5393 (an enabler, not a defect) |
| #5486 | other | SPEC-Q (rule) | X-spec | re-label | - | §35.3 choice payloads and inline arrays in the closure bullet |
| #5490 | other | SPEC-Q (rule) | X-spec | re-label | - | stdlib §2.3 says nothing about Convert.ToString(double) vs double.ToString() |
| #5854 | other | SPEC-Q (MEASURED) | X-spec | re-label | - | override Equals(Object?) on an enum/choice: §10.5 vs §9.2 |
| #5911 | crash | RE-LABEL (MEASURED) | B0-tier | re-label | N | §15.1 *OrNull tier unenforced (an inline-array key) |
| #5961 | other | RE-LABEL (rule) | X-13 | re-label | Y | static Equals/GetHashCode/ToString at another signature accepted (a USER member) |
| #5978 | crash | RE-LABEL (rule) | X-5 | re-label | - | a function value at Object is not recognised at run time |
| #5998 | other | REPRODUCES (rule) | B0-constraint | re-label | Y | Extension-head constraint check residuals over builtin heads |
| #6007 | other | RE-LABEL (rule) | X-13 | re-label | - | default(X) accepts built-in classes (§17.11) |
| #6024 | other | REPRODUCES (rule) | B5 | B5 | N | ?. on a builtin collection member infers a NON-nullable type (null receiver yields 0) |
| #6027 | other | RE-LABEL (rule) | B0-dx | re-label | - | ambiguous-call message lists Handle(IOException) twice |
| #6029 | other | RE-LABEL (rule) | B0-dx | re-label | - | two misleading diagnostics on an Object-trio override |
| #6035 | other | RE-LABEL (rule) | X-13 | re-label | - | default(T) over a composite record/choice chain (§17.11) |
| #6042 | other | RE-LABEL (rule) | X-7 | re-label | - | own-typed Equals on a generic owning choice fails LLVM verification; campaign 7 |
| #6049 | other | RE-LABEL (rule) | X-7 | re-label | - | generic choice falsely obliged GetHashCode (§8.3); campaign 7 |
| #6056 | crash | RE-LABEL (rule) | X-13 | re-label | - | a case type pattern at a function type emits malformed IR (§12.7) |
| #6058 | other | RE-LABEL (rule) | X-13 | re-label | - | a value at an Object-typed record component is stored unboxed |
| #6070 | other | REPRODUCES (rule) | B5 | B5 | N | chain continuation after a nullable-chain value segment is rejected (§7.5) |
| #6078 | other | RE-LABEL (rule) | X-13 | re-label | Y | override Equals(Object?) on a record/enum/choice accepted (§9.2) |
| #6084 | crash | RE-LABEL (MEASURED) | X-5 | re-label | - | `o as <choice>` then `<> null` fails IR verification; campaign 5 |
| #6100 | crash | RE-LABEL (rule) | X-11 | re-label | - | `return o ?? 5;` under Task<Object> emits malformed IR; campaign 11 |
| #6106 | other | RE-LABEL (rule) | X-13 | re-label | - | Object members on a function-typed receiver are left untyped |
| #6116 | crash | RE-LABEL (MEASURED) | X-5 | re-label | - | `bq ?? b = b` fails IR verification; campaign 5 |
| #6126 | crash | RE-LABEL (rule) | X-5 | re-label | - | nullable function value in an interpolation hole SEGVs; campaign 5 |
| #6151 | other | RE-LABEL (rule) | X-13 | re-label | - | GetHashCode on a choice listing an interface declaring it does not lower |
| #6159 | other | REPRODUCES (rule) | B0-symbol | re-label | Y | a user extension named Count/Length on a builtin receiver: the intrinsic answers |
| #6181 | other | RE-LABEL (rule) | X-7 | re-label | - | choiceHoldsBorrows reads a generic choice's payloads unsubstituted; campaign 7 |
| #6187 | other | RE-LABEL (rule) | X-13 | re-label | - | try/catch does not catch an exception from an Object-typed receiver's ToString |
| #6228 | other | RE-LABEL (rule) | X-7 | re-label | Y | interface witness picks the wrong overload for generic instantiations; campaign 7 |
| #6235 | other | RE-LABEL (rule) | X-lot | re-label | - | generic choice chain compile time is exponential |
| #6239 | other | RE-LABEL (rule) | X-7 | re-label | - | property read through a generic choice's interface prints 0; campaign 7 |
| #6253 | other | REPRODUCES (rule) | B0-amb | re-label | - | a USER static class S: E(Exception?)/E(ArgumentException?) is ambiguous (§22.6) — not a builtin call |
| #6254 | crash | REPRODUCES (rule) | B0-infer | re-label | - | a USER generic method M<T>(x: T?) — not a builtin call |
| #6255 | crash | REPRODUCES (rule) | B5 | B5 | N | narrowed int? at a long parameter fails IR verification (missing unwrap) |
| #6295 | crash | REPRODUCES (rule) | B0-typearg | re-label | Y | Math.Max<T> false rejection under a constraint; wrong type-arg count crashes |
| #6307 | other | RE-LABEL (rule) | X-7 | re-label | - | interface dispatch of a generic method runs the wrong body; campaign 7 |
| #6530 | other | REPRODUCES (rule) | B0-symbol | re-label | - | a type-parameter NAME guard over-rejects — not a builtin call |
| #6635 | crash | REPRODUCES (MEASURED) | B4 | B4a/B4b | Y | MEASURED grid: string.Contains(1) -> COMPILER ABORT (IR verification failed) |
| #6636 | soundness | REPRODUCES (MEASURED) | B4 | B4a/B4b | Y | MEASURED grid: string.Split(1) -> internal: permissive-acceptance note, prints 0 |
| #6637 | soundness | REPRODUCES (MEASURED) | B5 | B5 | N | MEASURED grid: Array<int>.IndexOf with a narrowed int? returns -1 |
| #6644 | other | RE-LABEL (MEASURED) | X-5 | re-label | - | a check-then-use of a captured nullable local is rejected (§7.4); campaign 5 |
| #6646 | crash | RE-LABEL (rule) | X-5 | re-label | - | a nullable choice payload is stored without unwrapping; campaign 5 |
| #6647 | crash | REPRODUCES (MEASURED) | B3 | B3a/B3b | Y | MEASURED grid: Math.Abs(decimal?) -> COMPILER ABORT |
| #6648 | soundness | REPRODUCES (rule) | B3 | B3a/B3b | N | the CALL IS LEGAL; Math.Min/Max/Clamp have an UNTYPED RESULT (the gate would not reject the call) |
| #6659 | crash | RE-LABEL (rule) | X-8 | re-label | - | Set<T?>/Dictionary<K?,V> with a nullable key; campaigns 5 and 8 |
| #6677 | crash | RE-LABEL (rule) | X-5 | re-label | - | `long? ?? int?` segfaults the compiler; campaign 5 |
| #6689 | crash | RE-LABEL (rule) | X-lot | re-label | - | FileStream at a Stream parameter fails IR verification |
| #6697 | other | RE-LABEL (MEASURED) | X-7 | re-label | - | generic inference binds T := int? from a narrowed read; campaign 7 |
| #6742 | other | REPRODUCES (rule) | B2 | B2a/B2b | N | TimeSpan.MinValue/MaxValue do not lower; ToString("c") rejected as Object.ToString arity |
| #6750 | crash | RE-LABEL (MEASURED) | X-5 | re-label | - | `in` over Array<long?> with an int operand fails IR verification; campaign 5 |
| #6751 | soundness | RE-LABEL (MEASURED) | X-13 | re-label | - | mixed-type `=` with byte/ulong against double gives wrong answers (§6.2) |
| #6753 | other | REPRODUCES (MEASURED) | B4 | B4a/B4b | N | MEASURED: a StringBuilder at Object prints bytes instead of its text |
| #6755 | other | REPRODUCES (MEASURED) | B4 | B4a/B4b | N | MEASURED: `o is StringBuilder` on an Object-held StringBuilder answers False |
| #6777 | other | REPRODUCES (rule) | B4 | B4a/B4b | N | interpolating a null StringBuilder? renders an empty string |
| #6780 | other | REPRODUCES (MEASURED) | B4 | B4a/B4b | Y | Debug.WriteLine accepts a StringBuilder and writes an empty line |
| #6782 | soundness | REPRODUCES (rule) | B6 | B6 | Y | TimeSpan.ToString(null)/DateTime.ToString(null) compile for a non-nullable format |
| #6809 | crash | REPRODUCES (rule) | B4 | B4a/B4b | Y | StringBuilder Insert/Remove/Replace accept wrong types and arities -> malformed IR |
| #6810 | other | REPRODUCES (rule) | B6 | B6 | Y | a named argument bypasses the null check on a non-nullable builtin string parameter |
| #6811 | other | REPRODUCES (rule) | B6 | B6 | Y | builtin non-string reference parameters declared non-nullable accept null |
| #6812 | other | REPRODUCES (rule) | B6 | B6 | Y | builtin non-nullable string parameters outside the #6782 table accept null |
| #6820 | other | REPRODUCES (MEASURED) | B4 | B4a/B4b | N | an Object local holding a StringBuilder misdispatches and SEGVs before reassignment |
| #6823 | soundness | REPRODUCES (MEASURED) | B5 | B5 | N | a narrowed nullable value at a builtin non-string overload passes the carrier |
| #6825 | other | REPRODUCES (MEASURED) | B6 | B6 | Y | long.ToString(null) is accepted (the #6782 map has no Int64 row) |
| #6826 | other | REPRODUCES (MEASURED) | B4 | B4a/B4b | Y | MEASURED grid: string.Contains(Object) accepted and answers True |
| #6845 | crash | REPRODUCES (MEASURED) | B0-symbol | re-label | Y | MEASURED: a user class named StringBuilder calling its own Insert -> SIGSEGV (rc -11) |
| #6854 | crash | RE-LABEL (MEASURED) | X-13 | re-label | - | a `case` expression with a binding type pattern emits malformed IR (§19.3) |
| #6860 | crash | RE-LABEL (MEASURED) | X-lot | re-label | - | the compiler segfaults on a `+` chain of 1500+ terms |
| #6864 | soundness | FIXED (MEASURED) | B3 | B3a/B3b | N | MEASURED: Math.Max(3u,4000000000u) now prints 4000000000 (was 3) |
| #6865 | soundness | FIXED (MEASURED) | B3 | B3a/B3b | N | MEASURED: Math.Min('a','b') now prints a (was 97) |
| #6872 | other | RE-LABEL (rule) | X-13 | re-label | - | an ASSIGNMENT (a builtin Type value at an int/string location), not a call |
| #6873 | crash | RE-LABEL (rule) | X-5 | re-label | - | null at a Type? parameter emits malformed IR; campaign 5 |
| #6881 | other | RE-LABEL (rule) | X-13 | re-label | - | a banked corpus row flipped after #6782 (a test row, not a defect) |
| #6884 | crash | REPRODUCES (MEASURED) | B3 | B3a/B3b | N | Math.Max/Min with mixed argument types -> malformed IR |
| #6892 | other | RE-LABEL (rule) | X-8 | re-label | - | identity-hash side-table entries are never forgotten; campaign 8 |
| #6933 | other | RE-LABEL (rule) | X-7 | re-label | - | x.ToString().Length at a call argument stops on #1386; campaign 7 |
| #6942 | soundness | REPRODUCES (MEASURED) | B3 | B3a/B3b | N | MEASURED grid: `var g := Math.Sign;` -> internal: degrade; Math.Sign<int> rejected |
| #6957 | soundness | REPRODUCES (MEASURED) | B4 | B4a/B4b | Y | MEASURED grid: Encoding.UTF8.GetString(Array<int>) is rejected today |
| #6983 | soundness | RE-LABEL (rule) | X-8 | re-label | - | FrozenArray<string>.From frees a borrow; campaign 8 |
| #7004 | soundness | RE-LABEL (MEASURED) | X-5 | re-label | - | a typed binding over an int? destructuring slot binds the carrier; campaign 5 |
| #7016 | other | REPRODUCES (rule) | B5 | B5 | N | ?. on a declared builtin-interface member emits nothing or zero-initializes |
| #7028 | other | REPRODUCES (rule) | B4 | B4a/B4b | N | Encoding.GetBytes/GetString through an Encoding-typed variable has no dispatch arm |
| #7030 | crash | RE-LABEL (rule) | X-lot | re-label | - | a lambda body reading q.IsEmpty on a Range<T> fails IR verification |
| #7061 | other | REPRODUCES (rule) | B3 | B3a/B3b | Y | Math.Abs(float) is typed float where the stdlib rationale says it resolves to double |
| #7066 | soundness | RE-LABEL (MEASURED) | X-7 | re-label | - | a same-module `var r: T? := x;` degrades; campaign 7 |
| #7089 | soundness | RE-LABEL (MEASURED) | X-5 | re-label | - | a §12.7-narrowed record carrier handed to a record consumer; campaign 5 |
| #7090 | other | RE-LABEL (MEASURED) | X-lot | re-label | - | a property reference over a builtin interface head is accepted and jumps into a value |
| #7093 | other | RE-LABEL (rule) | X-lot | re-label | - | a bare member reference: concrete head rejected, interface head admitted (§27.6) |
| #7098 | soundness | REPRODUCES (rule) | B0-arity | re-label | Y | a method group over a builtin member silently non-lowers on a signature mismatch |
| #7108 | soundness | FIXED (MEASURED) | B4 | B4a/B4b | N | MEASURED: the out-of-bounds read is GONE — "ab".Remove(253) raises IndexOutOfRangeException, which is what stdlib §9.6 requires; fixed by c0c9e3f9 (fix #7168), in origin/main |
| #7118 | crash | RE-LABEL (MEASURED) | X-lot | re-label | - | Array<T>.AsSpan(start) truncates a byte startIndex -> malformed IR |
| #7129 | other | REPRODUCES (MEASURED) | B4 | B4a/B4b | N | MEASURED grid: string.Repeat("ab",3) rejected - the declared string overload is not implemented |
| #7130 | other | REPRODUCES (rule) | B5 | B5 | N | the ?. spelling of the string instance methods is rejected in codegen (§7.5) |
| #7146 | other | RE-LABEL (rule) | X-1 | re-label | - | a builtin property read accepted as a Dispose() receiver; campaign 1 |
| #7149 | other | RE-LABEL (rule) | B0-docs | re-label | - | a stale measurement-table comment in TypeResolution_Names.cpp (a docs defect) |
| #7154 | crash | REPRODUCES (MEASURED) | B4 | B4a/B4b | N | StringBuilder.Replace lowers to a malformed bz_sb_replace call -> IR verification failed |
| #7167 | crash | REPRODUCES (MEASURED) | B4 | B4a/B4b | N | an out-of-bounds READ in the generated code (the type gate alone would not close it) |
| #7173 | other | REPRODUCES (MEASURED) | B0-typearg | re-label | Y | MEASURED grid: Console.WriteLine<int>("x") accepts ANY type-argument list and ignores it |
| #7180 | crash | MOVED (MEASURED) | B4 | B4a/B4b | Y | MEASURED: the compiler abort is GONE; Equals(reordered) is correct; CompareTo(reordered) is now REJECTED |
| #7189 | soundness | REPRODUCES (rule) | B4 | B4a/B4b | N | string.Join over an element wider than 8 bytes reads a truncated element |
| #7210 | soundness | REPRODUCES (MEASURED) | B4 | B4a/B4b | N | StringBuilder.Insert silently no-ops an out-of-range index (§3.1) |
| #7217 | crash | MOVED (MEASURED) | B0-symbol | re-label | Y | MEASURED: the SIGSEGV is gone, but the USER's own Replace is silently DROPPED (prints only 'after') |
| #7218 | crash | REPRODUCES (MEASURED) | B3 | B3a/B3b | Y | a Math/Convert argument is NEVER type-checked -> malformed IR instead of a §5.1 diagnostic |
| #7219 | other | REPRODUCES (MEASURED) | B3 | B3a/B3b | N | a Math/Convert member evaluates each argument TWICE (§22.5) |
| #7221 | other | REPRODUCES (rule) | B4 | B4a/B4b | N | StringBuilder.Insert/Remove silently no-op out of range (§3.1) |
| #7227 | other | REPRODUCES (MEASURED) | B3 | B3a/B3b | Y | MEASURED grid: Math.Sign("x") -> COMPILER ABORT; Math.Sign(true) -> -1 |
| #7228 | other | REPRODUCES (rule) | B4 | B4a/B4b | N | StringBuilder.Replace with an empty oldValue silently no-ops (§3.1) |
| #7229 | soundness | REPRODUCES (MEASURED) | B4 | B4a/B4b | N | the StringBuilder runtime mutators index BYTES while Length counts CODE POINTS |
| #7232 | crash | REPRODUCES (MEASURED) | B3 | B3a/B3b | Y | MEASURED grid: Math.Floor(double?) -> COMPILER ABORT |
| #7234 | other | REPRODUCES (rule) | B2 | B2a/B2b | Y | stdlib §2.3's ambiguity note is not implemented for Convert.ToString |
| #7239 | crash | REPRODUCES (MEASURED) | B2 | B2a/B2b | Y | MEASURED grid: a runtime long at Convert.ToString's toBase -> rejected; the CONSTANT spelling compiles |
| #7242 | other | FIXED (MEASURED) | B3 | B3a/B3b | N | MEASURED grid: Math.Sqrt(4000000000u) prints 63245.55 (was -nan) |
| #7244 | crash | MOVED (MEASURED) | B4 | B4a/B4b | N | MEASURED grid: StringBuilder.AppendFormat with a string argument no longer SEGVs |
| #7245 | other | REPRODUCES (rule) | B4 | B4a/B4b | N | String.IsAscii keys on the STORAGE WIDTH (§9.6) |
| #7246 | crash | RE-LABEL (rule) | X-lot | re-label | - | a local holding another local (a method reference) still SEGFAULTS |
| #7249 | other | REPRODUCES (MEASURED) | B3 | B3a/B3b | N | MEASURED grid: TimeSpan.FromTicks(4000000000u) prints -294967296 |
| #7252 | crash | MOVED (MEASURED) | B3 | B3a/B3b | N | MEASURED grid: DateTime.FromTicks(uint) no longer aborts; it prints -294967296 |
| #7253 | other | REPRODUCES (MEASURED) | B3 | B3a/B3b | N | ISteppable<T>.Step sign-extends an unsigned by |
| #7254 | other | REPRODUCES (MEASURED) | B3 | B3a/B3b | N | Math.Abs(4000000000u) rejected as ambiguous though §5.1 admits uint at long and double |
| #7258 | other | REPRODUCES (rule) | B1 | B1 | N | the radix rendering is wrong for unsigned values and above Int32.MaxValue |
| #7265 | crash | REPRODUCES (rule) | B2 | B2a/B2b | N | ToString(format) renders unsigned signed, ignores the binary spec, crashes for long/uint |
| #7271 | other | REPRODUCES (MEASURED) | B1 | B1 | Y | MEASURED grid: (255).ToString(16u) is accepted and renders the decimal text |
| #7274 | crash | REPRODUCES (rule) | B2 | B2a/B2b | N | a float and a double operand at Double.IeeeEquals/BitwiseEquals -> malformed IR |
| #7277 | other | REPRODUCES (MEASURED) | B4 | B4a/B4b | N | MEASURED grid: the declared static Repeat(unit: string, count: int) overload is not implemented |
| #7284 | crash | REPRODUCES (MEASURED) | B2 | B2a/B2b | Y | MEASURED grid: decimal.ToString(16) -> COMPILER ABORT (IR verification failed) |
| #7285 | crash | REPRODUCES (MEASURED) | B1 | B1 | Y | MEASURED grid: int.Parse("ff", true) is rejected; the uint radix is still accepted |
| #7286 | other | REPRODUCES (rule) | B1 | B1 | Y | the radix class is not closed (nullable, constant, string and runtime radices) |
| #7287 | crash | REPRODUCES (rule) | B2 | B2a/B2b | N | the literal format overload aborts for every non-32-bit integer receiver |
| #7290 | crash | MOVED (MEASURED) | B1 | B1 | Y | MEASURED: the abort is GONE; int.TryParse(16, out v) now compiles with an internal: degrade and prints False/0 |
| #7291 | soundness | REPRODUCES (rule) | B2 | B2a/B2b | Y | Convert.ToString aborts when the VALUE argument is a nullable |
| #7292 | other | REPRODUCES (rule) | B3 | B3a/B3b | N | the arguments of a named-argument call are evaluated twice (§22.5) |
| #7293 | crash | REPRODUCES (rule) | B1 | B1 | Y | double/float/decimal TryParse abort when the first argument is not a string |
| #7294 | soundness | REPRODUCES (rule) | B1 | B1 | Y | the one-argument int.Parse has no first-argument guard |
| #7295 | other | REPRODUCES (MEASURED) | B1 | B1 | Y | MEASURED grid: char.Parse("a",16) -> internal: silent drop instead of a located error |
| #7296 | other | REPRODUCES (rule) | B1 | B1 | Y | the named-argument spelling of a primitive ToString radix is owned by no nullability check |
| #7297 | crash | REPRODUCES (MEASURED) | B3 | B3a/B3b | Y | MEASURED grid: Math.Floor(double?) -> COMPILER ABORT (both narrowed and un-narrowed) |
| #7299 | crash | REPRODUCES (rule) | B1 | B1 | Y | the bool and char receivers of Parse/TryParse abort on a non-string first argument |
| #7300 | crash | MOVED (MEASURED) | B1 | B1 | Y | MEASURED: the abort is GONE; decimal.TryParse(1.5, out m) degrades silently (False/0) |
| #7303 | other | REPRODUCES (MEASURED) | B2 | B2a/B2b | N | MEASURED grid: (1.5).ToString("C") prints 1.5 instead of raising FormatException |
| #7304 | other | REPRODUCES (rule) | B1 | B1 | Y | `this`/`base` are still lowered as strings at int.TryParse |
| #7308 | other | RE-LABEL (rule) | B0-docs | re-label | - | the TryParse out-slot default is attributed to stdlib §9.3 in seven places (a citation class) |
| #7309 | other | RE-LABEL (rule) | B0-docs | re-label | - | stale stdlib line numbers in a test file |
| #7311 | other | REPRODUCES (rule) | B2 | B2a/B2b | N | the ToString(format) member-call path renders the no-form text |
| #7315 | other | REPRODUCES (rule) | B1 | B1 | Y | the parse-operand guard policy has three spellings and the radix arms disagree |
| #7319 | crash | REPRODUCES (rule) | B2 | B2a/B2b | Y | ToString(format) on a nullable receiver formats the carrier; a double? receiver aborts |
| #7320 | other | REPRODUCES (rule) | B1 | B1 | N | Guid.TryParse does not assign default(Guid) to its out slot on the false path |
| #7323 | other | REPRODUCES (rule) | B1 | B1 | Y | the parse-operand guards DROP the call silently where §22.6 says a compile error |
| #7325 | other | FIXED (rule) | B1 | B1 | - | CLOSED 2026-10-27 by b1adfab95 (in origin/main) |
| #7329 | crash | FIXED (rule) | B2 | B2a/B2b | - | CLOSED 2026-10-27 by 275fab52 (in origin/main) |
| #7331 | crash | REPRODUCES (rule) | B2 | B2a/B2b | Y | Convert.ToString on a nullable operand renders the carrier's bytes; the radix form aborts |
| #7333 | other | REPRODUCES (MEASURED) | B2 | B2a/B2b | Y | MEASURED grid: bool.ToString("F4") compiles (stdlib §2.18 says a compile error) |
| #7336 | other | REPRODUCES (rule) | B2 | B2a/B2b | N | a runtime format string on a bool/char receiver prints the plain rendering |
| #7340 | other | RE-LABEL (rule) | B2 | B2a/B2b | - | the JSON deserializer's number grammar is looser than JSON's (stdlib §2.15) |
| #7341 | other | FIXED (rule) | B2 | B2a/B2b | - | CLOSED 2026-10-27 by d9f8b36f (in origin/main) |
| #7342 | other | REPRODUCES (rule) | B2 | B2a/B2b | N | a narrowed Guid? receiver overflows the stack |
| #7358 | crash | REPRODUCES (rule) | B2 | B2a/B2b | N | a malformed format-specifier suffix on a narrow integer receiver emits invalid IR |
| #7359 | other | REPRODUCES (MEASURED) | B2 | B2a/B2b | N | MEASURED grid: (255).ToString("x") prints FF where §2.18 says lower-case hex |
| #7361 | other | REPRODUCES (rule) | B2 | B2a/B2b | N | the runtime-format spelling of X renders a 32-bit pattern for a narrow receiver |
| #7362 | other | REPRODUCES (rule) | B2 | B2a/B2b | N | D on a floating receiver truncates instead of raising FormatException |
