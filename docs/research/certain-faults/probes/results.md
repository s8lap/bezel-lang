# Probe results — bezelc 0.0.0-f6863e62 — 2026-10-27

## p00_k53b_baseline
- **checked**: compile=accept; run exit=70
  output:
      Unhandled exception: System.DisposedAccessException
- **unchecked**: compile=accept; run exit=139
  output:

## p01a_div_literal_zero
- **checked**: compile=accept; run exit=70
  output:
      Unhandled exception: System.DivideByZeroException
- **unchecked**: compile=accept; run exit=70
  output:
      Unhandled exception: System.DivideByZeroException

## p01b_mod_const_zero
- **checked**: compile=accept; run exit=70
  output:
      Unhandled exception: System.DivideByZeroException
- **unchecked**: compile=accept; run exit=70
  output:
      Unhandled exception: System.DivideByZeroException

## p01c_decimal_div_zero
- **checked**: compile=accept; run exit=70
  output:
      Unhandled exception: System.DivideByZeroException
- **unchecked**: compile=accept; run exit=70
  output:
      Unhandled exception: System.DivideByZeroException

## p01d_const_div_zero_6961
- **checked**: compile=accept; run exit=70
  output:
      Unhandled exception: System.DivideByZeroException
- **unchecked**: compile=accept; run exit=70
  output:
      Unhandled exception: System.DivideByZeroException

## p02_open_range_for
- **checked**: compile=REJECT
  diagnostics:
      p02_open_range_for.bzl:4:18: warning: open-ended range literal cannot be iterated (§16.5)
      p02_open_range_for.bzl:4:5: not yet implemented: codegen for this for statement
- **unchecked**: compile=REJECT
  diagnostics:
      p02_open_range_for.bzl:4:18: warning: open-ended range literal cannot be iterated (§16.5)
      p02_open_range_for.bzl:4:5: not yet implemented: codegen for this for statement

## p03a_requires_const_false
- **checked**: compile=accept; run exit=70
  output:
      Unhandled exception: System.ContractViolationException: requires precondition of Program.Half failed
- **unchecked**: compile=accept; run exit=70
  output:
      Unhandled exception: System.ContractViolationException: requires precondition of Program.Half failed

## p03b_requires_const_true
- **checked**: compile=accept; run exit=0
  output:
      4
- **unchecked**: compile=accept; run exit=0
  output:
      4

## p04a_inline_array_const_oob
- **checked**: compile=accept; run exit=70
  output:
      Unhandled exception: System.IndexOutOfRangeException
- **unchecked**: compile=accept; run exit=70
  output:
      Unhandled exception: System.IndexOutOfRangeException

## p04b_inline_array_const_oob_named
- **checked**: compile=accept; run exit=70
  output:
      Unhandled exception: System.IndexOutOfRangeException
- **unchecked**: compile=accept; run exit=70
  output:
      Unhandled exception: System.IndexOutOfRangeException

## p04c_const_string_oob
- **checked**: compile=accept; run exit=70
  output:
      Unhandled exception: System.IndexOutOfRangeException
- **unchecked**: compile=accept; run exit=70
  output:
      Unhandled exception: System.IndexOutOfRangeException

## p05a_owningtask_second_await_usingvar
- **checked**: compile=accept; run exit=70
  output:
      a
      Unhandled exception: System.InvalidOperationException
- **unchecked**: compile=accept; run exit=70
  output:
      a
      Unhandled exception: System.InvalidOperationException

## p05b_owningtask_second_await_owned
- **checked**: compile=REJECT
  diagnostics:
      p05b_owningtask_second_await_owned.bzl:15:20: `OwningTask<T>` `t` was already awaited (consumed) — an `OwningTask<T>` is single-consumer; awaiting it again moves out an already-moved result (§30.5 / §30.4)
- **unchecked**: compile=REJECT
  diagnostics:
      p05b_owningtask_second_await_owned.bzl:15:20: `OwningTask<T>` `t` was already awaited (consumed) — an `OwningTask<T>` is single-consumer; awaiting it again moves out an already-moved result (§30.5 / §30.4)

## p06_mutation_during_iteration
- **checked**: compile=accept-with-warning; run exit=70
  diagnostics:
      p06_mutation_during_iteration.bzl:6:7: warning: `Add` on `xs` modifies the collection this `for` is iterating; the enumerator is invalidated and the loop's next advance raises `InvalidOperationException` (§29.12). Collect the matches and remove them after the loop, or consume the collection with `PopAt`/`Pop` in a loop that does not iterate it.
  output:
      Unhandled exception: System.InvalidOperationException
- **unchecked**: compile=accept-with-warning; run exit=70
  diagnostics:
      p06_mutation_during_iteration.bzl:6:7: warning: `Add` on `xs` modifies the collection this `for` is iterating; the enumerator is invalidated and the loop's next advance raises `InvalidOperationException` (§29.12). Collect the matches and remove them after the loop, or consume the collection with `PopAt`/`Pop` in a loop that does not iterate it.
  output:
      Unhandled exception: System.InvalidOperationException

## p07_fresh_store_then_read
- **checked**: compile=accept-with-warning; run exit=70
  diagnostics:
      p07_fresh_store_then_read.bzl:13:12: warning: this `.Add(...)` store on `ws` stores a fresh unbound owned temporary into a borrowing-family collection: the argument dies at its K8 full-expression boundary (the end of this statement, §28.3), while a borrowing `Array` slot only stores a borrow and takes no ownership — so the collection retains a reference to a disposed instance, leaving the slot dangling; bind the source to a named holder first — `using var t := …;`, then store `t`, whose own lifetime the §28.11 store rule then checks, or use the owning family (`OwningArray`), whose store positions take ownership (§15.4 fresh-store warning, v0.243; §28.11)
  output:
      Unhandled exception: System.DisposedAccessException
- **unchecked**: compile=accept-with-warning; run exit=0
  diagnostics:
      p07_fresh_store_then_read.bzl:13:12: warning: this `.Add(...)` store on `ws` stores a fresh unbound owned temporary into a borrowing-family collection: the argument dies at its K8 full-expression boundary (the end of this statement, §28.3), while a borrowing `Array` slot only stores a borrow and takes no ownership — so the collection retains a reference to a disposed instance, leaving the slot dangling; bind the source to a named holder first — `using var t := …;`, then store `t`, whose own lifetime the §28.11 store rule then checks, or use the owning family (`OwningArray`), whose store positions take ownership (§15.4 fresh-store warning, v0.243; §28.11)
  output:
      

## p08a_bang_on_null_local
- **checked**: compile=accept; run exit=70
  output:
      Unhandled exception: System.NullReferenceException
- **unchecked**: compile=accept; run exit=70
  output:
      Unhandled exception: System.NullReferenceException

## p08b_bang_in_null_branch
- **checked**: compile=accept-with-warning; run exit=70
  diagnostics:
      p08b_bang_in_null_branch.bzl:13:5: warning: owned value in `w` is implicitly disposed at the end of its scope (§28.3 must-use / auto-drop); declare it `using var` to make the scope-bound disposal explicit, or `move` / `return` / `.Dispose()` / `_ := …` to discharge it
  output:
      Unhandled exception: System.NullReferenceException
- **unchecked**: compile=accept-with-warning; run exit=70
  diagnostics:
      p08b_bang_in_null_branch.bzl:13:5: warning: owned value in `w` is implicitly disposed at the end of its scope (§28.3 must-use / auto-drop); declare it `using var` to make the scope-bound disposal explicit, or `move` / `return` / `.Dispose()` / `_ := …` to discharge it
  output:
      Unhandled exception: System.NullReferenceException

## p09a_same_var_use_after_dispose
- **checked**: compile=accept-with-warning; run exit=70
  diagnostics:
      p09a_same_var_use_after_dispose.bzl:14:23: warning: use of `a` after a dominating explicit Dispose() call — use-after-dispose is undefined behaviour in unchecked mode and raises DisposedAccessException in checked mode (§28.2)
      p09a_same_var_use_after_dispose.bzl:13:5: explicit Dispose() of `a` was here
  output:
      Unhandled exception: System.DisposedAccessException
- **unchecked**: compile=accept-with-warning; run exit=139
  diagnostics:
      p09a_same_var_use_after_dispose.bzl:14:23: warning: use of `a` after a dominating explicit Dispose() call — use-after-dispose is undefined behaviour in unchecked mode and raises DisposedAccessException in checked mode (§28.2)
      p09a_same_var_use_after_dispose.bzl:13:5: explicit Dispose() of `a` was here
  output:

## p09b_second_dispose_harmless
- **checked**: compile=accept-with-warning; run exit=0
  diagnostics:
      p09b_second_dispose_harmless.bzl:14:5: warning: local `a` is disposed again here — the second `.Dispose()` is a no-op (disposal is idempotent, §28.2); the redundant call usually indicates a bug
  output:
      ok
- **unchecked**: compile=accept-with-warning; run exit=0
  diagnostics:
      p09b_second_dispose_harmless.bzl:14:5: warning: local `a` is disposed again here — the second `.Dispose()` is a no-op (disposal is idempotent, §28.2); the redundant call usually indicates a bug
  output:
      ok

## p10_field_overwrite_under_borrow
- **checked**: compile=accept; run exit=70
  output:
      Unhandled exception: System.DisposedAccessException
- **unchecked**: compile=accept; run exit=0
  output:
      

## p10b_field_dispose_under_borrow
- **checked**: compile=accept; run exit=70
  output:
      Unhandled exception: System.DisposedAccessException
- **unchecked**: compile=accept; run exit=0
  output:
      

## p11_first_over_minting_iterator
- **checked**: compile=accept; run exit=70
  output:
      Unhandled exception: System.DisposedAccessException
- **unchecked**: compile=accept; run exit=0
  output:
      

## p12_lazy_query_reenumeration
- **checked**: compile=REJECT
  diagnostics:
      p12_lazy_query_reenumeration.bzl:7:18: bound lazy query `q` was already consumed — a lazy LINQ pipeline is single-consumption (§29.11); use `.ToArray()` to materialise it for multiple uses (consumed again via for-in enumeration)
- **unchecked**: compile=REJECT
  diagnostics:
      p12_lazy_query_reenumeration.bzl:7:18: bound lazy query `q` was already consumed — a lazy LINQ pipeline is single-consumption (§29.11); use `.ToArray()` to materialise it for multiple uses (consumed again via for-in enumeration)

## p15_send_after_close
- **checked**: compile=accept; run exit=70
  output:
      Unhandled exception: System.Threading.ChannelClosedException
- **unchecked**: compile=accept; run exit=70
  output:
      Unhandled exception: System.Threading.ChannelClosedException

## p17_const_step_sign_mismatch
- **checked**: compile=accept-with-warning; run exit=0
  diagnostics:
      p17_const_step_sign_mismatch.bzl:4:18: warning: this range produces an empty iteration — the range's direction (lo <= hi) disagrees with the step's sign (§16.4)
  output:
      done
- **unchecked**: compile=accept-with-warning; run exit=0
  diagnostics:
      p17_const_step_sign_mismatch.bzl:4:18: warning: this range produces an empty iteration — the range's direction (lo <= hi) disagrees with the step's sign (§16.4)
  output:
      done

## p19_enum_const_cast_oor
- **checked**: compile=REJECT
  diagnostics:
      p19_enum_const_cast_oor.bzl:7:14: enum cast `Priority(99)` — 99 is not the underlying value of any declared constant of `Priority` (§5.4)
- **unchecked**: compile=REJECT
  diagnostics:
      p19_enum_const_cast_oor.bzl:7:14: enum cast `Priority(99)` — 99 is not the underlying value of any declared constant of `Priority` (§5.4)

## p20_shift_const_count_oor
- **checked**: compile=REJECT
  diagnostics:
      p20_shift_const_count_oor.bzl:6:28: shift count 40 is outside the domain `0..31` of the left operand's type `int` — a compile-time-constant count outside the domain is a compile error (§6.5/§17.14); write `x << (k & 31)` if the modular count is intended
- **unchecked**: compile=REJECT
  diagnostics:
      p20_shift_const_count_oor.bzl:6:28: shift count 40 is outside the domain `0..31` of the left operand's type `int` — a compile-time-constant count outside the domain is a compile error (§6.5/§17.14); write `x << (k & 31)` if the modular count is intended

## p21_const_narrow_oor
- **checked**: compile=REJECT
  diagnostics:
      p21_const_narrow_oor.bzl:4:14: the compile-time-constant operand `300` of the explicit conversion to `byte` would raise `OverflowException`; §5.2 rejects a faulting constant operand at the conversion site, not as a deferred runtime fault (§17.14, v0.300)
- **unchecked**: compile=REJECT
  diagnostics:
      p21_const_narrow_oor.bzl:4:14: the compile-time-constant operand `300` of the explicit conversion to `byte` would raise `OverflowException`; §5.2 rejects a faulting constant operand at the conversion site, not as a deferred runtime fault (§17.14, v0.300)

## p24_case_expr_const_scrutinee_nomatch
- **checked**: compile=accept-with-warning; run exit=70
  diagnostics:
      p24_case_expr_const_scrutinee_nomatch.bzl:4:14: warning: non-exhaustive `case`: scrutinee type cannot be proven exhaustive (add a `_` arm or cover the remaining value(s)) (§19.6)
  output:
      Unhandled exception: System.MatchFailureException
- **unchecked**: compile=accept-with-warning; run exit=70
  diagnostics:
      p24_case_expr_const_scrutinee_nomatch.bzl:4:14: warning: non-exhaustive `case`: scrutinee type cannot be proven exhaustive (add a `_` arm or cover the remaining value(s)) (§19.6)
  output:
      Unhandled exception: System.MatchFailureException

## p25_null_test_on_nonnullable
- **checked**: compile=accept-with-warning; run exit=0
  diagnostics:
      p25_null_test_on_nonnullable.bzl:12:5: warning: owned value in `w` is implicitly disposed at the end of its scope (§28.3 must-use / auto-drop); declare it `using var` to make the scope-bound disposal explicit, or `move` / `return` / `.Dispose()` / `_ := …` to discharge it
  output:
      a
- **unchecked**: compile=accept-with-warning; run exit=0
  diagnostics:
      p25_null_test_on_nonnullable.bzl:12:5: warning: owned value in `w` is implicitly disposed at the end of its scope (§28.3 must-use / auto-drop); declare it `using var` to make the scope-bound disposal explicit, or `move` / `return` / `.Dispose()` / `_ := …` to discharge it
  output:
      a

## p25b_coalesce_on_nonnullable
- **checked**: compile=accept-with-warning; run exit=0
  diagnostics:
      p25b_coalesce_on_nonnullable.bzl:13:5: warning: owned value in `o` is implicitly disposed at the end of its scope (§28.3 must-use / auto-drop); declare it `using var` to make the scope-bound disposal explicit, or `move` / `return` / `.Dispose()` / `_ := …` to discharge it
      p25b_coalesce_on_nonnullable.bzl:12:5: warning: owned value in `w` is implicitly disposed at the end of its scope (§28.3 must-use / auto-drop); declare it `using var` to make the scope-bound disposal explicit, or `move` / `return` / `.Dispose()` / `_ := …` to discharge it
  output:
      a
- **unchecked**: compile=accept-with-warning; run exit=0
  diagnostics:
      p25b_coalesce_on_nonnullable.bzl:13:5: warning: owned value in `o` is implicitly disposed at the end of its scope (§28.3 must-use / auto-drop); declare it `using var` to make the scope-bound disposal explicit, or `move` / `return` / `.Dispose()` / `_ := …` to discharge it
      p25b_coalesce_on_nonnullable.bzl:12:5: warning: owned value in `w` is implicitly disposed at the end of its scope (§28.3 must-use / auto-drop); declare it `using var` to make the scope-bound disposal explicit, or `move` / `return` / `.Dispose()` / `_ := …` to discharge it
  output:
      a

## p27a_owning_removeat_then_use
- **checked**: compile=accept-with-warning; run exit=70
  diagnostics:
      p27a_owning_removeat_then_use.bzl:17:23: warning: `w` borrows an element of `pool` that `pool.RemoveAt()` disposed (§28.11); this use raises `DisposedAccessException` in checked disposal mode and is undefined behaviour unchecked. Re-take the borrow after the mutation, take the element out first with `PopAt` / `Pop` / `PopValue`, or mutate after the last use.
      p27a_owning_removeat_then_use.bzl:16:5: disposing call on `pool` was here
  output:
      Unhandled exception: System.DisposedAccessException
- **unchecked**: compile=accept-with-warning; run exit=0
  diagnostics:
      p27a_owning_removeat_then_use.bzl:17:23: warning: `w` borrows an element of `pool` that `pool.RemoveAt()` disposed (§28.11); this use raises `DisposedAccessException` in checked disposal mode and is undefined behaviour unchecked. Re-take the borrow after the mutation, take the element out first with `PopAt` / `Pop` / `PopValue`, or mutate after the last use.
      p27a_owning_removeat_then_use.bzl:16:5: disposing call on `pool` was here
  output:
      

## p27b_owning_indexer_set_then_use
- **checked**: compile=accept; run exit=70
  output:
      Unhandled exception: System.DisposedAccessException
- **unchecked**: compile=accept; run exit=0
  output:
      

## p28_task_dispose_then_await
- **checked**: compile=accept; run exit=0
  output:
      5
- **unchecked**: compile=accept; run exit=139
  output:

## p29_enumerator_movenext_after_dispose
- **checked**: compile=accept-with-warning; run exit=0
  diagnostics:
      p29_enumerator_movenext_after_dispose.bzl:7:23: warning: use of `e` after a dominating explicit Dispose() call — use-after-dispose is undefined behaviour in unchecked mode and raises DisposedAccessException in checked mode (§28.2)
      p29_enumerator_movenext_after_dispose.bzl:6:5: explicit Dispose() of `e` was here
  output:
      True
- **unchecked**: compile=accept-with-warning; run exit=139
  diagnostics:
      p29_enumerator_movenext_after_dispose.bzl:7:23: warning: use of `e` after a dominating explicit Dispose() call — use-after-dispose is undefined behaviour in unchecked mode and raises DisposedAccessException in checked mode (§28.2)
      p29_enumerator_movenext_after_dispose.bzl:6:5: explicit Dispose() of `e` was here
  output:

## p37a_static_requires_const_false
- **checked**: compile=REJECT
  diagnostics:
      p37a_static_requires_const_false.bzl:4:21: a `static requires` clause is undischargeable at the proven tier when its expression names a parameter (§22.16 — the prover floor is constant expressions folding to `true`)
- **unchecked**: compile=REJECT
  diagnostics:
      p37a_static_requires_const_false.bzl:4:21: a `static requires` clause is undischargeable at the proven tier when its expression names a parameter (§22.16 — the prover floor is constant expressions folding to `true`)

## p37b_static_requires_nonconst
- **checked**: compile=REJECT
  diagnostics:
      p37b_static_requires_nonconst.bzl:4:21: a `static requires` clause is undischargeable at the proven tier when its expression names a parameter (§22.16 — the prover floor is constant expressions folding to `true`)
- **unchecked**: compile=REJECT
  diagnostics:
      p37b_static_requires_nonconst.bzl:4:21: a `static requires` clause is undischargeable at the proven tier when its expression names a parameter (§22.16 — the prover floor is constant expressions folding to `true`)

## p38_conditional_dispose_then_use
- **checked**: compile=REJECT
  diagnostics:
      p38_conditional_dispose_then_use.bzl:15:23: local `a` may have already been disposed on another branch and may not be read here (conditional-discharge join, §28.3 / §28.2 — confine every read of `a` to a single branch, or reassign it after the discharge to clear the obligation)
- **unchecked**: compile=REJECT
  diagnostics:
      p38_conditional_dispose_then_use.bzl:15:23: local `a` may have already been disposed on another branch and may not be read here (conditional-discharge join, §28.3 / §28.2 — confine every read of `a` to a single branch, or reassign it after the discharge to clear the obligation)
