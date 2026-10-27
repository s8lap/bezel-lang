# Results — bezelc 0.0.0-f6863e62 — 2026-10-27

## k53_p1_dispose_under_live_borrow
- **checked**: compile=accept; run exit=70
  output:
      dispose a
      Unhandled exception: System.DisposedAccessException
- **unchecked**: compile=accept; run exit=139
  output:
      dispose a

## k53_p2_dispose_after_last_use
- **checked**: compile=accept; run exit=0
  output:
      touch a
      dispose a
      done
- **unchecked**: compile=accept; run exit=0
  output:
      touch a
      dispose a
      done

## k53_p3_dispose_then_rebind_borrow
- **checked**: compile=accept-with-warning; run exit=70
  diagnostics:
      k53_p3_dispose_then_rebind_borrow.bzl:27:14: warning: use of `a` after a dominating explicit Dispose() call — use-after-dispose is undefined behaviour in unchecked mode and raises DisposedAccessException in checked mode (§28.2)
      k53_p3_dispose_then_rebind_borrow.bzl:26:5: explicit Dispose() of `a` was here
  output:
      dispose a
      Unhandled exception: System.DisposedAccessException
- **unchecked**: compile=accept-with-warning; run exit=139
  diagnostics:
      k53_p3_dispose_then_rebind_borrow.bzl:27:14: warning: use of `a` after a dominating explicit Dispose() call — use-after-dispose is undefined behaviour in unchecked mode and raises DisposedAccessException in checked mode (§28.2)
      k53_p3_dispose_then_rebind_borrow.bzl:26:5: explicit Dispose() of `a` was here
  output:
      dispose a

## k52_p1_overwrite_under_live_borrow
- **checked**: compile=REJECT
  diagnostics:
      k52_p1_overwrite_under_live_borrow.bzl:28:5: cannot consume `a` here: the borrow held by `b` is still live — its later use would read freed memory (consume-site freeze, §28.3/§28.7/§30.14 v0.180)
      k52_p1_overwrite_under_live_borrow.bzl:28:5: warning: owned value in `a` is implicitly disposed at the end of its scope (§28.3 must-use / auto-drop); declare it `using var` to make the scope-bound disposal explicit, or `move` / `return` / `.Dispose()` / `_ := …` to discharge it
- **unchecked**: compile=REJECT
  diagnostics:
      k52_p1_overwrite_under_live_borrow.bzl:28:5: cannot consume `a` here: the borrow held by `b` is still live — its later use would read freed memory (consume-site freeze, §28.3/§28.7/§30.14 v0.180)
      k52_p1_overwrite_under_live_borrow.bzl:28:5: warning: owned value in `a` is implicitly disposed at the end of its scope (§28.3 must-use / auto-drop); declare it `using var` to make the scope-bound disposal explicit, or `move` / `return` / `.Dispose()` / `_ := …` to discharge it

## k52_p2_unborrowed_reassignment
- **checked**: compile=accept-with-warning; run exit=0
  diagnostics:
      k52_p2_unborrowed_reassignment.bzl:27:5: warning: owned value in `a` is implicitly disposed at the end of its scope (§28.3 must-use / auto-drop); declare it `using var` to make the scope-bound disposal explicit, or `move` / `return` / `.Dispose()` / `_ := …` to discharge it
  output:
      touch first
      dispose first
      touch second
      dispose second
- **unchecked**: compile=accept-with-warning; run exit=0
  diagnostics:
      k52_p2_unborrowed_reassignment.bzl:27:5: warning: owned value in `a` is implicitly disposed at the end of its scope (§28.3 must-use / auto-drop); declare it `using var` to make the scope-bound disposal explicit, or `move` / `return` / `.Dispose()` / `_ := …` to discharge it
  output:
      touch first
      dispose first
      touch second
      dispose second

## k52_p3_dispose_then_overwrite_under_borrow
- **checked**: compile=accept-with-warning; run exit=70
  diagnostics:
      k52_p3_dispose_then_overwrite_under_borrow.bzl:29:5: warning: owned value in `a` is implicitly disposed at the end of its scope (§28.3 must-use / auto-drop); declare it `using var` to make the scope-bound disposal explicit, or `move` / `return` / `.Dispose()` / `_ := …` to discharge it
  output:
      dispose first
      dispose second
      Unhandled exception: System.DisposedAccessException
- **unchecked**: compile=accept-with-warning; run exit=0
  diagnostics:
      k52_p3_dispose_then_overwrite_under_borrow.bzl:29:5: warning: owned value in `a` is implicitly disposed at the end of its scope (§28.3 must-use / auto-drop); declare it `using var` to make the scope-bound disposal explicit, or `move` / `return` / `.Dispose()` / `_ := …` to discharge it
  output:
      dispose first
      second
      dispose second

## k52_p4_overwrite_in_loop_under_borrow
- **checked**: compile=REJECT
  diagnostics:
      k52_p4_overwrite_in_loop_under_borrow.bzl:31:7: cannot consume `a` here: the borrow held by `b` is still live — its later use would read freed memory (consume-site freeze, §28.3/§28.7/§30.14 v0.180)
      k52_p4_overwrite_in_loop_under_borrow.bzl:26:5: warning: owned value in `a` is implicitly disposed at the end of its scope (§28.3 must-use / auto-drop); declare it `using var` to make the scope-bound disposal explicit, or `move` / `return` / `.Dispose()` / `_ := …` to discharge it
- **unchecked**: compile=REJECT
  diagnostics:
      k52_p4_overwrite_in_loop_under_borrow.bzl:31:7: cannot consume `a` here: the borrow held by `b` is still live — its later use would read freed memory (consume-site freeze, §28.3/§28.7/§30.14 v0.180)
      k52_p4_overwrite_in_loop_under_borrow.bzl:26:5: warning: owned value in `a` is implicitly disposed at the end of its scope (§28.3 must-use / auto-drop); declare it `using var` to make the scope-bound disposal explicit, or `move` / `return` / `.Dispose()` / `_ := …` to discharge it

## k52_p5_dispose_then_overwrite_in_loop
- **checked**: compile=accept-with-warning; run exit=70
  diagnostics:
      k52_p5_dispose_then_overwrite_in_loop.bzl:27:5: warning: owned value in `a` is implicitly disposed at the end of its scope (§28.3 must-use / auto-drop); declare it `using var` to make the scope-bound disposal explicit, or `move` / `return` / `.Dispose()` / `_ := …` to discharge it
  output:
      w0
      dispose w0
      dispose w1
      Unhandled exception: System.DisposedAccessException
- **unchecked**: compile=accept-with-warning; run exit=0
  diagnostics:
      k52_p5_dispose_then_overwrite_in_loop.bzl:27:5: warning: owned value in `a` is implicitly disposed at the end of its scope (§28.3 must-use / auto-drop); declare it `using var` to make the scope-bound disposal explicit, or `move` / `return` / `.Dispose()` / `_ := …` to discharge it
  output:
      w0
      dispose w0
      w1
      dispose w1
      dispose w2

## k313_p1_unmarked_read_is_borrow
- **checked**: compile=accept; run exit=0
  output:
      touch a
      touch a
      dispose a
- **unchecked**: compile=accept; run exit=0
  output:
      touch a
      touch a
      dispose a

## k313_p2_owned_argument_needs_move
- **checked**: compile=REJECT
  diagnostics:
      k313_p2_owned_argument_needs_move.bzl:27:23: warning: owned parameter `w` is implicitly disposed at the end of the method (§28.3 must-use / auto-drop); `move` / `return` / `.Dispose()` it, or adopt it with `using var w2 := move w;` (the `move` marker is required — an unmarked `using var` rebind is a borrow-pin, §28.10), to discharge it explicitly
      k313_p2_owned_argument_needs_move.bzl:36:13: this call consumes `w`; write `M(move w)` — passing the named owned binding `w` to an `owned` parameter transfers ownership and leaves it moved-from, which the call site must announce (§22.3, v0.286; §17.8 v0.309)
- **unchecked**: compile=REJECT
  diagnostics:
      k313_p2_owned_argument_needs_move.bzl:27:23: warning: owned parameter `w` is implicitly disposed at the end of the method (§28.3 must-use / auto-drop); `move` / `return` / `.Dispose()` it, or adopt it with `using var w2 := move w;` (the `move` marker is required — an unmarked `using var` rebind is a borrow-pin, §28.10), to discharge it explicitly
      k313_p2_owned_argument_needs_move.bzl:36:13: this call consumes `w`; write `M(move w)` — passing the named owned binding `w` to an `owned` parameter transfers ownership and leaves it moved-from, which the call site must announce (§22.3, v0.286; §17.8 v0.309)

## k313_p3_owned_argument_with_move
- **checked**: compile=accept-with-warning; run exit=0
  diagnostics:
      k313_p3_owned_argument_with_move.bzl:27:23: warning: owned parameter `w` is implicitly disposed at the end of the method (§28.3 must-use / auto-drop); `move` / `return` / `.Dispose()` it, or adopt it with `using var w2 := move w;` (the `move` marker is required — an unmarked `using var` rebind is a borrow-pin, §28.10), to discharge it explicitly
  output:
      touch w
      dispose w
      after
- **unchecked**: compile=accept-with-warning; run exit=0
  diagnostics:
      k313_p3_owned_argument_with_move.bzl:27:23: warning: owned parameter `w` is implicitly disposed at the end of the method (§28.3 must-use / auto-drop); `move` / `return` / `.Dispose()` it, or adopt it with `using var w2 := move w;` (the `move` marker is required — an unmarked `using var` rebind is a borrow-pin, §28.10), to discharge it explicitly
  output:
      touch w
      dispose w
      after

## k313_p4_plain_argument_is_borrow
- **checked**: compile=accept; run exit=0
  output:
      log w
      log w
      touch w
      dispose w
- **unchecked**: compile=accept; run exit=0
  output:
      log w
      log w
      touch w
      dispose w
