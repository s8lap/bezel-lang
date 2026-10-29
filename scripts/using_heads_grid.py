#!/usr/bin/env python3
"""using_heads_grid.py — the campaign-1 (`using-heads`) DONE grid.

THE GRID. A cell is (head kind) x (binding form). The head kind is what the
`using var` / `await using` binding's RIGHT-HAND SIDE carries; the binding form
is the spelling the program uses. Every cell has ONE expectation, taken from the
spec, and the grid is GREEN only when every cell's observed behaviour matches
its expectation.

WHY THIS SHAPE. It is `scripts/regression_matrix.py`'s shape: a deterministic
generator of .bzl programs, compiled with `bezelc --sanitize=address`, run, and
classified into CLEAN / LEAK / CRASH / CF / TIMEOUT. The difference is the
oracle: regression_matrix compares two builds against each other ("worse than
the reference"); this grid compares one build against the SPEC, per cell, and
prints how many cells are wrong.

    scripts/using_heads_grid.py --bezelc build/bezelc --out /var/tmp/... \
        [--filter class,string] [--jobs 8] [--timeout 60] [--report FILE]

Exit status: 0 when no cell blocks the DONE verdict (no WRONG, no
KNOWN-RED, no MISMATCH, no TIMEOUT), 1 when at least one cell blocks it, 2 on
a usage or infrastructure error.

THE THREE DIMENSIONS. A cell is (head kind) x (binding form) x (source kind),
and the third dimension is spelled out rather than implicit:

  * HEAD KIND — what the right-hand side carries (a class, a class behind a
    base, an interface-typed head, an abstract head, a generic head, a string,
    a function value, a value-like choice, an owning choice, an `Object`-
    declared head, a `Type` token, a built-in collection, a `Task`).
  * BINDING FORM — the spelling: `using var`, `await using var`, an explicit
    `Dispose()`, an explicit `DisposeAsync()`, a COPY of a source binding, an
    owned field store.
  * SOURCE KIND — for the COPY form only, where the copy's SOURCE comes from:
    a `var` local read by the plain `using var` spelling, the same `var` local
    read by the `await using var` spelling, a §21.1 `const` local, or a
    PARAMETER. These are four different programs with four different
    obligations, so they are four cells. Collapsing them into one cell would
    leave the `const`, the parameter and the `await using` variants unpinned: a
    future change could regress any of them without the grid noticing. (The
    `const` and parameter variants are #7316's and #7357's shapes; the `await
    using` spelling of the copy is #7374's.)

THE EXPECTATIONS. Each cell carries the spec sentence that decides it; the
sentence is quoted in the cell's comment and in the record
(`docs/proposals/papers/campaigns/using-heads.md`). The load-bearing ones:

  §28.10  "For ordinary value types and runtime-managed types, the disposal at
           scope end is a no-op (these types do not require disposal per
           §28.9), and the only effect is the reassignability restriction."
  §28.12  "`await using var` takes §28.10's admission unchanged ... Over an
           owned right-hand side with no `DisposeAsync` to await ... scope exit
           takes the synchronous disposal (or the no-op) and likewise awaits
           nothing."
  §28.12  "The `DisposeAsync` awaited is the run-time object's. `await using`
           selects the logical-disposal path by the class of the object the
           binding holds ... never by the binding's static type. ... through
           `Object`, and through any interface ... Only an object whose class
           does not implement `IAsyncDisposable` takes the synchronous disposal
           above."
  §28.12  step 1: "For `await using`, the awaited `DisposeAsync()` performs
           whatever asynchronous cleanup the class defines; the destructor body
           (if any) is not also run, because `DisposeAsync` supersedes it."
  §28.12  step 2: "owned fields are disposed in reverse declaration order and
           the base class's reclamation chains, once, immediately after logical
           disposal has completed".
  §28.9   "None of them is a class, and none has a `Dispose()` member:
           `x.Dispose()` over a value of one of these types is a compile error."
  §28.2   "`Dispose()` on any class instance is idempotent: the first call
           performs the disposal; subsequent calls are no-ops."
  §21.1   "The initializer of a runtime `const` must not be an owned production."

ORACLE NOTES (the house's traps, honoured here):

  * EVERY cell that can make a leak claim LOOPS its operation LOOP_N times.
    A one-shot LSan repro is vacuous: the dead frame's stale pointer reads as
    a root, so a one-shot program can report nothing while the leak is real.
    A cell that claims "must not leak" needs the loop too, or it cannot fail.
    LOOP_N is 60, inside the 50-500 band.
  * Every RUN-family cell asserts the program's PRINTED OUTPUT, not just its
    exit code: a program that printed the wrong thing is a different defect
    from one that leaked, and a compile-time rejection must not masquerade as
    a clean run.
  * A VAC cell asserts the §28.9 vacuity warning IS present (the compiler
    knows the binding carries no obligation) AND that the program runs clean.
    A NOVAC cell asserts it is ABSENT.
  * Every non-REJ cell must be CLEAN: no leak, no crash, no sanitizer report.
  * A cell that is red because a NAMED issue is still open is reported as
    KNOWN-RED with that issue. A KNOWN-RED cell BLOCKS the DONE verdict just
    like a WRONG one does (a red cell is red, whoever owns it), and a cell
    that is red for any other reason is WRONG. The campaign is DONE when no
    WRONG, no KNOWN-RED, no MISMATCH and no TIMEOUT cell is left.
  * The ASan leg must actually run. Every cell's program is built with
    `--sanitize=address`, and the summary prints
    `NATIVE SANITIZER: <n> check(s) skipped`, where `<n>` is the number of
    cells whose ASan check was skipped because the ASan RUNTIME was
    unavailable — measured by inspecting each built executable for the
    runtime (see `asan_skipped_count`). A program that fails to COMPILE or
    to LINK is a REJECTED cell (a WRONG cell when the cell expected a run):
    there is no SKIPPED verdict, and such a cell blocks the DONE verdict on
    its own, so nothing is hidden by the sanitizer counter.
  * A COMPILE TIMEOUT is its own verdict (TIMEOUT), it is NOT a sanitizer
    skip, and it BLOCKS the DONE verdict: a cell whose program hung the
    compiler is UNMEASURED, and a DONE instrument must not print YES with an
    unmeasured cell.

Every type and member used here exists in docs/Bezel_Standard_Library.md; the
sections are cited at the head table. Do not add a member that is not there; a
stdlib gap is an issue, not a generator patch.
"""
import argparse
import concurrent.futures
import os
import re
import subprocess
import sys

CLEAN, LEAK, CRASH, CF, TIMEOUT = ('CLEAN', 'LEAK', 'CRASH', 'CF', 'TIMEOUT')

# The leak-oracle band (50-500). Every non-REJ cell loops this many times.
LOOP_N = 60

# ---------------------------------------------------------------------------
# The head table. Every entry:
#   (label, decl, rhs, extra_preamble, param_type)
#
#   decl   the declared type of the binding (None = inferred)
#   rhs    the right-hand side expression
#   extra  extra declarations the head needs
#   ptype  the type of the PARAMETER the copy's source is passed as (the
#          copy's third dimension)
# ---------------------------------------------------------------------------

PRELUDE = '''import System.Threading;

public class Res
  public constructor()
  end constructor;

  destructor
    Console.WriteLine("DTOR");
  end destructor;
end Res;

public class AsyncRes: IAsyncDisposable
  public constructor()
  end constructor;

  public async method DisposeAsync(): Task
    await Task.Delay(0);
    Console.WriteLine("DA");
  end DisposeAsync;

  destructor
    Console.WriteLine("DTOR");
  end destructor;
end AsyncRes;

public class Base
  public constructor()
  end constructor;

  destructor
    Console.WriteLine("BASE_DTOR");
  end destructor;
end Base;

public class Der: Base, IAsyncDisposable
  public constructor()
  end constructor;

  public async method DisposeAsync(): Task
    await Task.Delay(0);
    Console.WriteLine("DA");
  end DisposeAsync;

  destructor
    Console.WriteLine("DER_DTOR");
  end destructor;
end Der;

public abstract class Abs: IAsyncDisposable
  public constructor()
  end constructor;

  public abstract method DisposeAsync(): Task;

  destructor
    Console.WriteLine("ABS_DTOR");
  end destructor;
end Abs;

public class AbsD: Abs
  public constructor()
  end constructor;

  public async override method DisposeAsync(): Task
    await Task.Delay(0);
    Console.WriteLine("DA");
  end DisposeAsync;

  destructor
    Console.WriteLine("ABSD_DTOR");
  end destructor;
end AbsD;

public choice VChoice Note(text: string), Empty; end VChoice;

public choice OChoice Text(owned r: Res), Empty; end OChoice;
'''

# head key -> (label, ty, rhs, extra, ptype)
HEADS = {
    'class': (
        'class (own destructor, no IAsyncDisposable)',
        None, 'new Res()', '', 'Res'),
    'class_async': (
        'class implementing IAsyncDisposable',
        None, 'new AsyncRes()', '', 'AsyncRes'),
    'derived_base': (
        'derived behind a base that has no DisposeAsync',
        'Base', 'new Der()', '', 'Base'),
    'interface': (
        'interface-typed head (IAsyncDisposable)',
        'IAsyncDisposable', 'new AsyncRes()', '', 'IAsyncDisposable'),
    'abstract': (
        'abstract-class head with an abstract DisposeAsync',
        'Abs', 'new AbsD()', '', 'Abs'),
    'generic_class': (
        'generic T, class-constrained (new T() at T = Res)',
        None, 'new T()', '', 'T'),
    'generic_unconstrained': (
        'generic T, new()-constrained at T = Stopwatch (vtable-less head)',
        None, 'new T()', '', 'T'),
    'string': (
        'string (runtime-managed, §28.9)',
        None, 'o.ToString()', '', 'string'),
    'function': (
        'function value (runtime-managed, §28.9)',
        None, 'lambda(): int => 1', '', '(int) -> int'),
    'value_choice': (
        'value-like choice (§35.4)',
        None, 'VChoice.Note("a" + "b")', '', 'VChoice'),
    'owning_choice': (
        'owning choice (§35.4)',
        None, 'OChoice.Text(move osrc)', '', 'OChoice'),
    'object_class': (
        'Object holding a class',
        'Object', 's', '', 'Object'),
    'object_string': (
        'Object holding a runtime-managed string',
        'Object', 's', '', 'Object'),
    'type_token': (
        'Type token (§28.9) - typeof is deferred (§17.10, #487)',
        'Object', 'typeof(Int32)', '', 'Object'),
    'builtin_collection': (
        'built-in collection (Array<int>)',
        'Array<int>', 'new Array<int>(3, 0)', '', 'Array<int>'),
    'collection_literal': (
        'built-in collection LITERAL (an owned producer, §28.3)',
        'Array<int>', '[1, 2, 3]', '', 'Array<int>'),
    'task': (
        'Task (a borrow read of tcs.Task)',
        None, 'tcs.Task', '', 'Task<int>'),
}

# The extra declarations each head needs beyond PRELUDE, and the setup lines
# each head's program needs before the binding under test.
HEAD_SETUP = {
    'string': '    var o := 1;\n',
    'function': '',
    'value_choice': '',
    'owning_choice': '    var osrc := new Res();\n',
    'object_class': '    var s: Object := new Res();\n',
    'object_string': '    var o := 1;\n    var s: Object := o.ToString();\n',
    'type_token': '',
    'builtin_collection': '',
    'collection_literal': '',
    'task': '    using var tcs := new TaskCompletionSource<int>();\n',
    'generic_class': '',
    'generic_unconstrained': '',
}

# The parameter DECLARATION the copy's source is passed as. A move-only head's
# parameter must be `owned` (§15.3: a move-only value is passed BY VALUE-MOVE,
# and the `move` call-site marker is only valid on an `owned` parameter).
PARAM_DECL = {
    'class': 'src: Res',
    'class_async': 'src: AsyncRes',
    'derived_base': 'src: Base',
    'interface': 'src: IAsyncDisposable',
    'abstract': 'src: Abs',
    'generic_class': 'src: T',
    'generic_unconstrained': 'src: T',
    'string': 'src: string',
    'function': 'src: () -> int',
    'value_choice': 'src: VChoice',
    'owning_choice': 'owned src: OChoice',
    'object_class': 'src: Object',
    'object_string': 'src: Object',
    'type_token': 'src: Object',
    'builtin_collection': 'src: Array<int>',
    'collection_literal': 'src: Array<int>',
    'task': 'src: Task<int>',
}

# Heads whose setup must live OUTSIDE the loop (the Task head's
# TaskCompletionSource has to outlive the loop: the loop's first iteration's
# scope-end disposal awaits the uncompleted task -- that is #6194's hang).
SETUP_OUTSIDE_LOOP = {'task'}

# The generic heads run through a monomorphized wrapper, so the binding under
# test lives INSIDE the wrapper's method and the program body is a call.
GENERIC_CALL = {
    'generic_class': '    await GW<Res>.Run();',
    'generic_unconstrained': '    await GU<Stopwatch>.Run();',
}

GENERIC_DECL = {
    'generic_class': '',
    'generic_unconstrained': 'import System.Diagnostics;\n',
}

GENERIC_CLASS = {
    'generic_class': 'GW',
    'generic_unconstrained': 'GU',
}

# A holder with an owned field, for the `field store` binding form.
FIELD_HOLDER = {
    'class': 'public class Holder\n'
             '  public owned F: Res? := null;\n'
             '  public constructor() end constructor;\n'
             'end Holder;\n',
    'class_async': 'public class Holder\n'
                   '  public owned F: AsyncRes? := null;\n'
                   '  public constructor() end constructor;\n'
                   'end Holder;\n',
    'derived_base': 'public class Holder\n'
                    '  public owned F: Base? := null;\n'
                    '  public constructor() end constructor;\n'
                    'end Holder;\n',
    'interface': 'public class Holder\n'
                 '  public owned F: IAsyncDisposable? := null;\n'
                 '  public constructor() end constructor;\n'
                 'end Holder;\n',
    'abstract': 'public class Holder\n'
                '  public owned F: Abs? := null;\n'
                '  public constructor() end constructor;\n'
                'end Holder;\n',
    'builtin_collection': 'public class Holder\n'
                          '  public owned F: Array<int>? := null;\n'
                          '  public constructor() end constructor;\n'
                          'end Holder;\n',
    'collection_literal': 'public class Holder\n'
                          '  public owned F: Array<int>? := null;\n'
                          '  public constructor() end constructor;\n'
                          'end Holder;\n',
    'task': 'public class Holder\n'
            '  public owned F: Task<int>? := null;\n'
            '  public constructor() end constructor;\n'
            'end Holder;\n',
}

# ---------------------------------------------------------------------------
# The binding forms.
#
# `copy_of_using_var` is spelled `copy_local_source` / `copy_const_source` /
# `copy_param_source` because the copy's SOURCE is a third dimension: a `var`
# local, a §21.1 `const` local, or a parameter. See the module docstring.
# ---------------------------------------------------------------------------

FORMS = [
    ('using_var', 'using var'),
    ('await_using_var', 'await using var'),
    ('explicit_dispose', 'explicit Dispose()'),
    ('explicit_dispose_async', 'explicit DisposeAsync()'),
    ('copy_local_source', 'copy of a `var` local source'),
    ('copy_await_source', 'copy of a `var` local source, `await using` spelling'),
    ('copy_const_source', 'copy of a `const` source'),
    ('copy_param_source', 'copy of a parameter source'),
    ('field_store', 'owned field store'),
]

# ---------------------------------------------------------------------------
# Expectations.
#
#   ('REJ', needle)   the compile must be REJECTED with a diagnostic holding
#                     `needle`
#   ('RUN', markers)  the program must compile, run CLEAN under ASan, and print
#                     exactly `markers` (once per loop iteration)
#   ('VAC', markers)  as RUN, and the §28.9 vacuity warning must be present
#   ('NOVAC', markers) as RUN, and the §28.9 vacuity warning must be ABSENT
#   None              the spec does not decide the cell (a spec question); the
#                     cell is reported as UNDECIDED and is NOT counted wrong
#
# A third element, when present, is the list of lines the program prints AFTER
# the loop (the generic heads' "body" marker).
# ---------------------------------------------------------------------------

EXPECT = {
    # -- class (own destructor, no IAsyncDisposable) -------------------------
    # §28.10: "the disposal at scope end is a no-op" applies to VALUE types and
    # runtime-managed types only; a class head disposes its destructor chain.
    ('class', 'using_var'): ('RUN', ['DTOR']),
    # §28.12: "an object whose class does not implement `IAsyncDisposable` --
    # scope exit takes the synchronous disposal (or the no-op) and likewise
    # awaits nothing."
    ('class', 'await_using_var'): ('RUN', ['DTOR']),
    ('class', 'explicit_dispose'): ('RUN', ['DTOR']),
    ('class', 'explicit_dispose_async'): ('REJ', ['§11.3', '§12.1', '§28.12']),
    # §28.10 "Adopting a named owned binding": an unmarked `using var y := x;`
    # over a named owned binding whose declared type is a class is a compile
    # error.
    ('class', 'copy_local_source'): ('REJ', '§28.10'),
    # §21.1: "The initializer of a runtime `const` must not be an owned
    # production" -- `new Res()` is one.
    ('class', 'copy_const_source'): ('REJ', '§21.1'),
    # A PARAMETER is not a "named owned binding" (§28.10's definition lists a
    # `var`/`using var` local, an owned iteration variable, an `out owned`
    # recipient, an owning-match arm binding, or an `owned` parameter), so the
    # unmarked copy of a parameter is a BORROW-PIN: no disposal, and the
    # caller keeps the obligation. The destructor runs once, in the caller.
    ('class', 'copy_param_source'): ('RUN', ['DTOR']),
    ('class', 'field_store'): ('RUN', ['DTOR']),

    # -- class implementing IAsyncDisposable --------------------------------
    # §28.12 step 1: "the awaited `DisposeAsync()` performs whatever
    # asynchronous cleanup the class defines; the destructor body (if any) is
    # not also run, because `DisposeAsync` supersedes it."
    ('class_async', 'using_var'): ('RUN', ['DTOR']),
    ('class_async', 'await_using_var'): ('RUN', ['DA']),
    ('class_async', 'explicit_dispose'): ('RUN', ['DTOR']),
    # §28.12 step 2: "a direct `DisposeAsync()` call awaited outside `await
    # using` ... the cascade and the chain follow exactly as if `await using`
    # had issued it" -- so the block must be reclaimed.
    ('class_async', 'explicit_dispose_async'): ('RUN', ['DA']),
    ('class_async', 'copy_local_source'): ('REJ', '§28.10'),
    ('class_async', 'copy_const_source'): ('REJ', '§21.1'),
    ('class_async', 'copy_param_source'): ('RUN', ['DTOR']),
    ('class_async', 'field_store'): ('RUN', ['DTOR']),

    # -- derived behind a base that has no DisposeAsync (§28.12's own example)
    ('derived_base', 'using_var'): ('RUN', ['DER_DTOR', 'BASE_DTOR']),
    # §28.12: "`await using var b: Base := new Der();` with `class Der: Base,
    # IAsyncDisposable` awaits `Der.DisposeAsync`, skips `Der`'s destructor and
    # runs `Base`'s."
    ('derived_base', 'await_using_var'): ('RUN', ['DA', 'BASE_DTOR']),
    ('derived_base', 'explicit_dispose'): ('RUN', ['DER_DTOR', 'BASE_DTOR']),
    ('derived_base', 'explicit_dispose_async'): ('REJ', ['§11.3', '§12.1', '§28.12']),
    ('derived_base', 'copy_local_source'): ('REJ', '§28.10'),
    ('derived_base', 'copy_const_source'): ('REJ', '§21.1'),
    ('derived_base', 'copy_param_source'): ('RUN', ['DER_DTOR', 'BASE_DTOR']),
    ('derived_base', 'field_store'): ('RUN', ['DER_DTOR', 'BASE_DTOR']),

    # -- interface-typed head (IAsyncDisposable) ----------------------------
    ('interface', 'using_var'): ('RUN', ['DTOR']),
    # §28.12: "through `Object`, and through any interface -- `IAsyncDisposable`
    # or one deriving from it, whose `DisposeAsync` dispatches as any interface
    # member does".
    ('interface', 'await_using_var'): ('RUN', ['DA']),
    ('interface', 'explicit_dispose'): ('RUN', ['DTOR']),
    ('interface', 'explicit_dispose_async'): ('RUN', ['DA']),
    ('interface', 'copy_local_source'): ('REJ', '§28.10'),
    ('interface', 'copy_const_source'): ('REJ', '§21.1'),
    ('interface', 'copy_param_source'): ('RUN', ['DTOR']),
    # §28.5: the owned field's cascade disposes the stored instance.
    ('interface', 'field_store'): ('RUN', ['DTOR']),

    # -- abstract-class head with an abstract DisposeAsync -------------------
    ('abstract', 'using_var'): ('RUN', ['ABSD_DTOR', 'ABS_DTOR']),
    # §28.12 step 2: "the destructor body is skipped if that level declares or
    # overrides a non-`abstract` `DisposeAsync`, and runs if that level does not
    # -- ... a level whose `DisposeAsync` is `abstract` ... [is] cleaned up
    # exactly as on the synchronous path."
    ('abstract', 'await_using_var'): ('RUN', ['DA', 'ABS_DTOR']),
    ('abstract', 'explicit_dispose'): ('RUN', ['ABSD_DTOR', 'ABS_DTOR']),
    ('abstract', 'explicit_dispose_async'): ('RUN', ['DA', 'ABS_DTOR']),
    ('abstract', 'copy_local_source'): ('REJ', '§28.10'),
    ('abstract', 'copy_const_source'): ('REJ', '§21.1'),
    ('abstract', 'copy_param_source'): ('RUN', ['ABSD_DTOR', 'ABS_DTOR']),
    ('abstract', 'field_store'): ('RUN', ['ABSD_DTOR', 'ABS_DTOR']),

    # -- generic T ----------------------------------------------------------
    # The binding lives inside the monomorphized wrapper; "body" is printed
    # after the loop.
    ('generic_class', 'using_var'): ('RUN', ['DTOR'], ['body']),
    ('generic_class', 'await_using_var'): ('RUN', ['DTOR'], ['body']),
    # A bare type parameter is a class-or-interface type in the generic body
    # (§28.1 clause 3), so `Dispose`/`DisposeAsync` are not members of it.
    ('generic_class', 'explicit_dispose'): ('REJ', '§14.2'),
    ('generic_class', 'explicit_dispose_async'): ('REJ', '§14.2'),
    # §28.10's bare-type-parameter clause: "using var y := x; over an `owned
    # x: T` is rejected once, in the body".
    ('generic_class', 'copy_local_source'): ('REJ', '§28.10'),
    ('generic_class', 'copy_const_source'): ('REJ', '§21.1'),
    ('generic_class', 'copy_param_source'): ('RUN', ['DTOR']),
    ('generic_class', 'field_store'): None,

    ('generic_unconstrained', 'using_var'): ('RUN', [], ['body']),
    ('generic_unconstrained', 'await_using_var'): ('RUN', [], ['body']),
    ('generic_unconstrained', 'explicit_dispose'): ('REJ', '§14.2'),
    ('generic_unconstrained', 'explicit_dispose_async'): ('REJ', '§14.2'),
    ('generic_unconstrained', 'copy_local_source'): ('REJ', '§28.10'),
    ('generic_unconstrained', 'copy_const_source'): ('REJ', '§21.1'),
    ('generic_unconstrained', 'copy_param_source'): ('RUN', []),
    ('generic_unconstrained', 'field_store'): None,

    # -- runtime-managed (§28.9) ----------------------------------------------
    # §28.9: "a `using var` local of function type is nonetheless permitted
    # (disposal at scope end is a no-op)"; §28.10: "the disposal at scope end is
    # a no-op ... and the only effect is the reassignability restriction".
    ('string', 'using_var'): ('VAC', []),
    ('string', 'await_using_var'): ('VAC', []),
    # §28.9: "none has a `Dispose()` member ... `x.Dispose()` over a value of one
    # of these types is a compile error".
    ('string', 'explicit_dispose'): ('REJ', '§28.9'),
    ('string', 'explicit_dispose_async'): ('REJ', ['§28.9', '§28.12']),
    ('string', 'copy_local_source'): ('VAC', []),
    ('string', 'copy_const_source'): ('VAC', []),
    ('string', 'copy_param_source'): ('VAC', []),
    ('string', 'field_store'): None,

    ('function', 'using_var'): ('VAC', []),
    ('function', 'await_using_var'): ('VAC', []),
    ('function', 'explicit_dispose'): ('REJ', '§28.9'),
    ('function', 'explicit_dispose_async'): ('REJ', ['§27.1', '§28.9', '§28.12']),
    ('function', 'copy_local_source'): ('VAC', []),
    ('function', 'copy_const_source'): ('VAC', []),
    ('function', 'copy_param_source'): ('VAC', []),
    ('function', 'field_store'): None,

    # -- value-like / owning choice (§35.4) ----------------------------------
    # §28.10: "the disposal at scope end is a no-op" -- but the RUNTIME
    # LIFETIME must be the plain-`var` twin's (the §35.3 construction's +1 on
    # the payload string must be released at scope end either way). The twin is
    # measured CLEAN for this head (see `--twins`), so "must not leak" IS
    # "same lifetime as the plain-var twin".
    ('value_choice', 'using_var'): ('VAC', []),
    ('value_choice', 'await_using_var'): ('VAC', []),
    ('value_choice', 'explicit_dispose'): ('REJ', '§35.8'),
    ('value_choice', 'explicit_dispose_async'): ('REJ', ['§11.3', '§12.1', '§28.12']),
    ('value_choice', 'copy_local_source'): ('VAC', []),
    ('value_choice', 'copy_const_source'): ('VAC', []),
    ('value_choice', 'copy_param_source'): ('VAC', []),
    ('value_choice', 'field_store'): None,

    # §35.4: an owning choice's active case's `owned` payload IS disposed at
    # scope end; and §28.10: "Where `x` is a local of move-only value type ...
    # the unmarked form **adopts**" -- so the copy of a move-only local is
    # ADMITTED and carries the payload to the copy's scope end (the source is
    # left moved-from).
    ('owning_choice', 'using_var'): ('RUN', ['DTOR']),
    ('owning_choice', 'await_using_var'): ('RUN', ['DTOR']),
    ('owning_choice', 'explicit_dispose'): ('REJ', '§35.8'),
    ('owning_choice', 'explicit_dispose_async'): ('REJ', ['§11.3', '§12.1', '§28.12']),
    ('owning_choice', 'copy_local_source'): ('RUN', ['DTOR']),
    # §21.1: a move-only value is REJECTED as a `const` initializer.
    ('owning_choice', 'copy_const_source'): ('REJ', '§21.1'),
    # §15.3: "parameter passing is by value-move" -- the callee owns the value
    # and the unmarked copy adopts it.
    ('owning_choice', 'copy_param_source'): ('RUN', ['DTOR']),
    ('owning_choice', 'field_store'): None,

    # -- Object-declared ------------------------------------------------------
    # EVERY run-family cell of this head is NOVAC, not RUN (the exit review's
    # FINDING 11, 2026-10-28). The head's runtime value is a CLASS instance
    # (`new Res()`), and §28.9 says of its exempt set that "None of them is a
    # class": `Object` is not one of the runtime-managed types
    # (`string`, `Type`, boxed value types, function values), so the §28.10
    # lint's premise -- "the RHS is a value-typed/runtime-managed value with no
    # disposal obligation (§28.9)" -- is false of this binding, and §28.12
    # selects the disposal by "the class of the object the binding holds ...
    # never by the binding's static type". A plain RUN does not look at the
    # warning, so the false §28.9 vacuity warning that the compiler draws here
    # was invisible to the DONE verdict.
    ('object_class', 'using_var'): ('NOVAC', ['DTOR']),
    # §28.12: "`await using var` takes §28.10's admission unchanged". §28.10's
    # "Adopting a named owned binding" rejection needs a CLASS or INTERFACE
    # declared type; `s` is declared `Object`, which is neither (§8.7), so the
    # binding is a BORROW-PIN and the program is ADMITTED -- exactly as the
    # plain `using var` twin of the same program is. The two spellings must
    # agree; today the `await using` spelling REJECTS the program the plain
    # spelling admits.
    ('object_class', 'await_using_var'): ('NOVAC', ['DTOR']),
    # §8.7: "`Dispose()` is class-only ... On `Object` ... the name resolves to no
    # member, so a call in any spelling ... [is a] compile error".
    ('object_class', 'explicit_dispose'): ('REJ', '§8.7'),
    ('object_class', 'explicit_dispose_async'): ('REJ', ['§8.7', '§28.12']),
    ('object_class', 'copy_local_source'): ('NOVAC', ['DTOR']),
    ('object_class', 'copy_const_source'): ('NOVAC', ['DTOR']),
    ('object_class', 'copy_param_source'): ('NOVAC', ['DTOR']),
    ('object_class', 'field_store'): None,

    ('object_string', 'using_var'): ('TWIN', []),
    ('object_string', 'await_using_var'): ('TWIN', []),
    ('object_string', 'explicit_dispose'): ('REJ', '§8.7'),
    ('object_string', 'explicit_dispose_async'): ('REJ', ['§8.7', '§28.12']),
    ('object_string', 'copy_local_source'): ('TWIN', []),
    ('object_string', 'copy_const_source'): ('TWIN', []),
    ('object_string', 'copy_param_source'): ('TWIN', []),
    ('object_string', 'field_store'): None,

    # -- Type token (typeof is deferred) --------------------------------------
    ('type_token', 'using_var'): ('REJ', '§17.10'),
    ('type_token', 'await_using_var'): ('REJ', '§17.10'),
    ('type_token', 'explicit_dispose'): ('REJ', '§17.10'),
    ('type_token', 'explicit_dispose_async'): ('REJ', '§17.10'),
    ('type_token', 'copy_local_source'): ('REJ', '§17.10'),
    ('type_token', 'copy_const_source'): ('REJ', '§17.10'),
    ('type_token', 'copy_param_source'): ('REJ', '§17.10'),
    ('type_token', 'field_store'): ('REJ', '§17.10'),

    # -- built-in collection --------------------------------------------------
    # A collection literal is an owned producer (§28.3's producers table), so a
    # `using var` over one draws NO §28.9 vacuity warning.
    ('builtin_collection', 'using_var'): ('NOVAC', []),
    ('builtin_collection', 'await_using_var'): ('NOVAC', []),
    ('builtin_collection', 'explicit_dispose'): ('NOVAC', []),
    ('builtin_collection', 'explicit_dispose_async'): ('REJ', ['§22.5', '§28.12']),
    ('builtin_collection', 'copy_local_source'): ('REJ', '§28.10'),
    ('builtin_collection', 'copy_const_source'): ('REJ', '§21.1'),
    ('builtin_collection', 'copy_param_source'): ('NOVAC', []),
    ('builtin_collection', 'field_store'): ('NOVAC', []),

    # -- built-in collection LITERAL -----------------------------------------
    # A collection literal is an OWNED PRODUCER (§28.3's producers table), so a
    # `using var` over one draws NO §28.9 vacuity warning -- the corpus case
    # s28_10_0016 (#7092) says the compiler draws one anyway.
    ('collection_literal', 'using_var'): ('NOVAC', []),
    ('collection_literal', 'await_using_var'): ('NOVAC', []),
    ('collection_literal', 'explicit_dispose'): ('NOVAC', []),
    ('collection_literal', 'explicit_dispose_async'): ('REJ', ['§22.5', '§28.12']),
    ('collection_literal', 'copy_local_source'): ('REJ', '§28.10'),
    ('collection_literal', 'copy_const_source'): ('REJ', '§21.1'),
    ('collection_literal', 'copy_param_source'): ('NOVAC', []),
    ('collection_literal', 'field_store'): ('NOVAC', []),

    # -- Task -----------------------------------------------------------------
    ('task', 'using_var'): ('NOVAC', []),
    ('task', 'await_using_var'): ('NOVAC', []),
    ('task', 'explicit_dispose'): ('NOVAC', []),
    ('task', 'explicit_dispose_async'): ('REJ', ['§8.7', '§28.12']),
    ('task', 'copy_local_source'): ('NOVAC', []),
    ('task', 'copy_const_source'): ('NOVAC', []),
    ('task', 'copy_param_source'): ('NOVAC', []),
    ('task', 'field_store'): ('NOVAC', []),
}

# The `await using` spelling of the copy takes §28.10's admission UNCHANGED
# (§28.12), so it has the SAME expectation as the plain copy -- and its own
# defect (#7374: the await spelling is admitted but never armed, so the copy's
# reference is never released). The entries are derived, not re-typed, so the
# two spellings can never drift apart -- and the derivation is a COPY of the
# expectation, never the agreement itself: the agreement is checked by
# `check_agreement`, which compares the two cells' OBSERVED verdicts.
#
# The derived needle is the plain cell's rule PLUS §28.12: the await spelling
# is rejected under §28.10's rule, and a diagnostic that cites §28.12
# instead (the rule that carries the admission UNCHANGED) decides the same
# cell. A §28.3 must-use diagnostic cites NEITHER, so the await cell cannot
# be green on the wrong rule again.
for _h in list(HEADS):
    if (_h, 'copy_local_source') in EXPECT:
        _plain = EXPECT[(_h, 'copy_local_source')]
        if _plain is None:
            EXPECT[(_h, 'copy_await_source')] = None
        elif _plain[0] == 'REJ':
            _needle = _plain[1]
            if not isinstance(_needle, (list, tuple)):
                _needle = [_needle]
            EXPECT[(_h, 'copy_await_source')] = (
                'REJ', list(_needle) + ['§28.12'])
        else:
            EXPECT[(_h, 'copy_await_source')] = _plain

# Cells that are red TODAY because a NAMED issue is still open. The issue is
# the one that owns the fix; a cell whose red is attributable to it is
# reported as KNOWN-RED — and a KNOWN-RED cell BLOCKS the DONE verdict just
# like a WRONG one does (see the DONE criterion in `judge`).
#
# A KNOWN-RED row MUST name the commit that fixes the cell, and that commit
# must exist in the repository with the issue number in its subject. The
# reviewer's demonstration: with an arbitrary string in KNOWN_RED and no
# tracker check, `--filter value_choice` printed "5 known-red" and EXITED 0
# with five cells RED AND LEAKING. A row that cannot name a fixing commit is
# not a KNOWN-RED row at all: the cell is WRONG, and the record carries the
# attribution. (#7374 and #7357 have no fixing commit yet, so their cells are
# WRONG and the campaign's U1c lane owns them.)
KNOWN_RED = {
    # (head, form) -> (issue, needle, fixing commit)
    ('value_choice', 'copy_local_source'): ('#7316', 'LEAK', 'b6053a68'),
    ('value_choice', 'copy_const_source'): ('#7316', 'LEAK', 'b6053a68'),
}

# Cells QUARANTINED from the DONE count. The `task` row hangs RUN-TO-RUN on the
# SAME build: this is a REAL RUNTIME RACE in the compiled program (issue #7395;
# measured 2 of 30 runs under ASan and 4 of 30 plain runs hang, the Task
# scope-end disposal of #6194), NOT a flaky oracle. The grid's runner and
# its program are deterministic; the same executable completes on one run and
# hangs on the next (the reviewer's C11(d) measured 100 ok / 39 wrong on one
# run and 99 ok / 40 wrong on the next, the flipping cell being
# `task/await_using_var`). A DONE instrument whose verdict flips run-to-run
# is not an instrument, so the row is counted separately and REPORTED as its
# own line until the race is fixed (#6194 / #7395). A quarantined cell never
# makes the DONE verdict worse and never makes it better. The race is a
# DEFECT to fix, not noise to filter: do not raise the timeout or re-run
# until green.
QUARANTINE = {
    # (head, form) -> the issue whose landing lifts the quarantine
    ('task', 'using_var'): '#6194',
    ('task', 'await_using_var'): '#6194',
    ('task', 'explicit_dispose'): '#6194',
    ('task', 'explicit_dispose_async'): '#6194',
    ('task', 'copy_local_source'): '#6194',
    ('task', 'copy_await_source'): '#6194',
    ('task', 'copy_const_source'): '#6194',
    ('task', 'copy_param_source'): '#6194',
    ('task', 'field_store'): '#6194',
}

# Cells that are red TODAY because of a defect another CAMPAIGN owns (the R37
# plan's section 4 ruling; the exit review's rule "a red cell is red, whoever
# owns it" was written for cells a campaign-1 lane could fix). A FOREIGN cell is
# SHOWN in the table (its row reads `FOREIGN(#N)` and keeps the underlying
# verdict in its text) but is held OUT of the DONE count: campaign 1 cannot
# exit before four other campaigns land otherwise. It never makes the verdict
# worse; and it never makes it better -- it is reported on its own line.
#
# A FOREIGN row is honest only while the cell is red FOR THAT ISSUE:
#   * a FOREIGN cell that turns OK is reported OK with a note to delete its row;
#   * `--check-issues` re-promotes a FOREIGN cell whose issue is CLOSED to a
#     blocking WRONG cell (the issue claims done and the cell disagrees);
#   * only WRONG / MISMATCH / KNOWN-RED are demoted: a TIMEOUT is UNMEASURED and
#     keeps blocking.
#
# Each entry was checked against the cell's observed text and the issue:
#   #7288  the function head's four copy cells LEAK (3840B/60 in the loop
#          oracle): a closure-value ownership defect the issue names, not a
#          `using var` admission or disposal defect;
#   #7092  a collection LITERAL is an owned producer (§28.3.4) but the ownership
#          pass does not see it as one: no must-use warning on the `var src`
#          holding it (compare the `builtin_collection` twin), the `const`
#          initializer is not refused (§21.1), and `using var` is called vacuous;
#   #7241  the vacuity lint's guard `!rhsIsDisposableClassType(rhsTy)`
#          (MethodOwnership::handleLocalDecl) answers `classifyDisposal(...)
#          .carriesObligation`, false for the borrowing family -- the predicate
#          #7241 names. The cell is the BORROW-PIN form (a callee `using var x
#          := src` over `src: Array<int>`), not #7241's owned-holder literal
#          repro, so the attribution is CONFIRMED BY CODE READING of the shared
#          predicate, not by a fix;
#   #7273  `string` `Dispose()` / `DisposeAsync()` are refused, but the
#          diagnostic names no §28.9 (the rule the cell asserts).
FOREIGN = {
    # (head, form) -> the issue (owned by another campaign) that explains the red
    ('function', 'copy_local_source'): '#7288',
    ('function', 'copy_await_source'): '#7288',
    ('function', 'copy_const_source'): '#7288',
    ('function', 'copy_param_source'): '#7288',
    ('collection_literal', 'using_var'): '#7092',
    ('collection_literal', 'await_using_var'): '#7092',
    ('collection_literal', 'copy_await_source'): '#7092',
    ('collection_literal', 'copy_const_source'): '#7092',
    ('collection_literal', 'copy_param_source'): '#7092',
    ('builtin_collection', 'copy_param_source'): '#7241',
    ('string', 'explicit_dispose'): '#7273',
    ('string', 'explicit_dispose_async'): '#7273',
}

# §28.12: "`await using var` takes §28.10's admission UNCHANGED". The two
# spellings of the same program must AGREE on the admission. The grid used to
# DERIVE the await cell's expectation from the plain cell's, which means a
# lane that relaxed the plain cell's expectation relaxed both — the grid could
# not see a disagreement at all. These pairs are compared instead: the two
# cells' ADMISSION verdicts (rejected / not rejected) must match, and a
# mismatch is its own verdict.
#
# A head in OPEN_QUESTION is NOT judged here: the spec question is the OWNER's,
# and the grid must not settle it by adding a verdict of its own.
# `object_class` is that head (§28.12's "unchanged" against §28.10's "Adopting
# a named owned binding" over an `Object`-declared binding — the record's open
# question 1). Its two cells keep the verdicts the record's reading gives
# them, and the record's question and its held-out hunk stay as they are.
AGREEMENT_PAIRS = [
    ('using_var', 'await_using_var'),
    ('copy_local_source', 'copy_await_source'),
]
OPEN_QUESTION = {'object_class'}


# ---------------------------------------------------------------------------
# Program construction
# ---------------------------------------------------------------------------


def _bind(ty, rhs, form):
    """The binding statement(s) for a form. Returns a list of source lines."""
    ann = f': {ty}' if ty else ''
    if form == 'using_var':
        return [f'    using var x{ann} := {rhs};']
    if form == 'await_using_var':
        return [f'    await using var x{ann} := {rhs};']
    if form == 'explicit_dispose':
        return [f'    var x{ann} := {rhs};', '    x.Dispose();']
    if form == 'explicit_dispose_async':
        return [f'    var x{ann} := {rhs};', '    await x.DisposeAsync();']
    if form == 'copy_local_source':
        return [f'    var src{ann} := {rhs};', '    using var x := src;']
    if form == 'copy_await_source':
        return [f'    var src{ann} := {rhs};', '    await using var x := src;']
    if form == 'copy_const_source':
        return [f'    const src{ann} := {rhs};', '    using var x := src;']
    if form == 'field_store':
        return ['    var h := new Holder();', f'    h.F := {rhs};']
    raise KeyError(form)


def modname(head, form):
    """A unique module name per program: the AOT bincache and the link step
    both key on the module name, so two programs sharing one would collide."""
    return 'G' + ''.join(ch if ch.isalnum() else '_' for ch in f'{head}_{form}')


def _wrap_loop(lines, n=LOOP_N):
    """Put the binding lines inside a counting loop.

    THE LEAK ORACLE. A one-shot LSan repro is vacuous (the dead frame's stale
    pointer reads as a root), so every cell that can make a leak claim loops.
    """
    inner = [ln.replace('    ', '      ') for ln in lines]
    return (['    var i := 0;', f'    while i < {n} do'] + inner +
            ['      i := i + 1;', '    end;'])


def _loop_body(setup, lines, n=LOOP_N):
    """The loop body: the head's SETUP and the binding, one iteration each.

    The setup lives INSIDE the loop so that every iteration builds a FRESH
    right-hand side: a head whose source is consumed (`move osrc`) or whose
    value is a per-iteration allocation needs one per iteration for the leak
    accounting to mean anything.
    """
    inner = []
    if setup:
        inner.extend(setup.rstrip('\n').split('\n'))
    inner.extend(lines)
    return _wrap_loop(inner, n)


def _param_source_program(head, ty, rhs, extra, setup):
    """The `copy_param_source` program: the copy's SOURCE is a PARAMETER.

    The callee receives the value as a plain parameter -- a BORROW, so the
    callee's `using var x := src;` is a borrow-pin and disposes nothing
    (§28.10's "`using var` initialized from a borrow"). The caller keeps the
    obligation and disposes the value itself, so the destructor (if any) runs
    in the caller.

    A class-typed head's caller-side disposal is spelled `src.Dispose();`; an
    `Object`-declared head's is the scope-end auto-disposal (§8.7: `Object` has
    no `Dispose` member); a move-only head's is the callee's, because a
    move-only value is passed BY VALUE-MOVE (§15.3).
    """
    label, _, _, _, ptype = HEADS[head]
    call = 'Take(src);'
    dispose = ''
    if head in ('class', 'class_async', 'derived_base', 'interface',
                'abstract', 'builtin_collection'):
        dispose = 'src.Dispose();'
    elif head == 'owning_choice':
        # The value is move-only: it is passed BY VALUE-MOVE (§15.3), so the
        # callee owns it and disposes it; the caller must not.
        call = 'Take(move src);'
    src_decl = f'var src{": " + ty if ty else ""} := {rhs};'
    lines = [src_decl, call] + ([dispose] if dispose else [])
    body = _loop_body(setup, lines)
    decls = PRELUDE + extra
    decls += (f'public class Program\n'
              f'  public static method Take({PARAM_DECL[head]})\n'
              f'    using var x := src;\n'
              f'  end Take;\n'
              f'  public static async method Main(): Task\n'
              + '\n'.join(body) + '\n'
              '    Console.WriteLine("END");\n'
              '  end Main;\n'
              'end Program;\n')
    return ('module ' + modname(head, 'copy_param_source') + ';\n' + decls)


def _generic_param_program(head):
    """The `copy_param_source` program for a monomorphized head: the whole
    thing lives inside the wrapper, because the call site's type is the
    substituted type."""
    cls = GENERIC_CLASS[head]
    call = GENERIC_CALL[head].replace('await ', '').replace('.Run()', '.Take()')
    call = call.strip()
    decls = PRELUDE
    decls += (f'public class {cls}<T> where T: new()\n'
              '  public static method Take()\n'
              '    var i := 0;\n'
              f'    while i < {LOOP_N} do\n'
              '      var src := new T();\n'
              '      Inner(src);\n'
              '      i := i + 1;\n'
              '    end;\n'
              '  end Take;\n'
              '  public static method Inner(src: T)\n'
              '    using var x := src;\n'
              '  end Inner;\n'
              f'end {cls};\n')
    body = [call, '    Console.WriteLine("END");']
    return ('module ' + modname(head, 'copy_param_source') + ';\n' +
            GENERIC_DECL.get(head, '') + decls +
            'public class Program\n'
            '  public static async method Main(): Task\n' +
            '\n'.join(body) + '\n'
            '  end Main;\n'
            'end Program;\n')


# ---------------------------------------------------------------------------
# The EXTENDED cells (R37 lane C1-L7, spec v0.350).
#
# The 153-cell grid is HEADS x FORMS. The cells below are the SHAPES the
# campaign's open members live in that the product grid has no axis for:
# an owned-RETURNING CALL as the head (#3483, #7417), the ADOPTION of an
# `owned` parameter (#7481), the nullable owning-choice parameter (#7486), the
# pin / move-out consume-site rules (#7405, #7406), the PER-LEVEL reclamation
# of a hierarchy (#3515), an `IOwningCollection<T>` local (#7463) and the
# iterator-frame heads (#2623, #7422). Each is one (head, form) cell with a
# hand-written program (`EXTRA`), listed in the table next to the 153.
#
# EVERY expectation here is read off a QUOTED spec sentence (v0.350), never
# off what the compiler does today; the compiler's present behaviour is only
# used to check that the PROGRAM is a valid one (a CONTROL cell -- the same
# program minus the offending line, or the corrected spelling -- must RUN).
#
# EXTRA[(head, form)] = (decls, members, setup, lines, expectation)
#   decls    declarations after PRELUDE
#   members  extra members of `Program` (static helpers)
#   setup    lines run before the binding, INSIDE the loop
#   lines    the binding lines under test
# ---------------------------------------------------------------------------

EXTRA_HEADS = {
    'call_rhs_class': 'owned-returning call head: Make(): owned Res',
    'call_rhs_class_async':
        'owned-returning call head: Make(): owned AsyncRes',
    'call_rhs_interface':
        'owned-returning call head: Make(): owned IAsyncDisposable (#3483)',
    'call_rhs_derived_base':
        'owned-returning call head: Make(): owned Base over Der (#7417)',
    'owned_param': '`owned` class parameter adopted by a using var (#7481)',
    'owned_param_choice':
        '`owned` owning-choice parameter (#7415 control for #7486)',
    'owned_param_nullable':
        '`owned` NULLABLE owning-choice parameter (#7486)',
    'pin_move_out':
        'pin and move-out of a using var (#7405, #7406; consume-site freeze)',
    'per_level_reclaim':
        'per-level reclamation chain, owned field in a derived level (#3515)',
    'owning_collection_local':
        'IOwningCollection<T> local from an owned-returning call (#7463)',
    'async_enumerator': 'IAsyncEnumerator<int> frame with a finally (#2623)',
    'sync_enumerator': 'IEnumerator<int> frame in an owned field (#7422)',
    'async_enumerable': 'IAsyncEnumerable<int> in an owned field (#7422)',
}


def _call_rhs(ret, body):
    """The four owned-returning-call heads share one shape: a static
    `Make(): owned T` and the `using var` / `await using var` over its call.

    §28.10: the right-hand side of a `using var` that is a method call WITHOUT
    an `owned` return is a borrow (the pin case); a call WITH an `owned`
    return is an owned production, so the `using var` takes over the
    disposal, exactly as it does for a `new` expression. And §28.12: "`await
    using` selects the logical-disposal path by the class of the object the
    binding holds ... never by the binding's static type" -- so the producer
    (a `new` or a call) decides nothing about which cleanup runs, and the
    expectation is the `new` head's.
    """
    return (f'  public static method Make(): owned {ret}\n'
            f'    return {body};\n'
            '  end Make;\n')


EXTRA = {}


def _x(head, form, expect, lines, decls='', members='', setup=''):
    EXTRA[(head, form)] = (decls, members, setup, lines, expect)
    EXPECT[(head, form)] = expect


for _head, _ret, _body, _um, _am in (
        ('call_rhs_class', 'Res', 'new Res()', ['DTOR'], ['DTOR']),
        # §28.12 step 1: "the awaited `DisposeAsync()` performs whatever
        # asynchronous cleanup the class defines; the destructor body (if any)
        # is not also run, because `DisposeAsync` supersedes it".
        ('call_rhs_class_async', 'AsyncRes', 'new AsyncRes()',
         ['DTOR'], ['DA']),
        # §28.12: "through any interface -- `IAsyncDisposable` ... whose
        # `DisposeAsync` dispatches as any interface member does" (#3483:
        # the residual is the CALL producer; the `new` producer is OK).
        ('call_rhs_interface', 'IAsyncDisposable', 'new AsyncRes()',
         ['DTOR'], ['DA']),
        # §28.12: "`await using var b: Base := new Der();` with `class Der:
        # Base, IAsyncDisposable` awaits `Der.DisposeAsync`, skips `Der`'s
        # destructor and runs `Base`'s" (#7417: the same through a CALL).
        ('call_rhs_derived_base', 'Base', 'new Der()',
         ['DER_DTOR', 'BASE_DTOR'], ['DA', 'BASE_DTOR'])):
    _m = _call_rhs(_ret, _body)
    _x(_head, 'using_var', ('RUN', _um), ['    using var x := Program.Make();'],
       members=_m)
    _x(_head, 'await_using_var', ('RUN', _am),
       ['    await using var x := Program.Make();'], members=_m)

# -- #7481: adopting an `owned` parameter ------------------------------------
# §28.10 "Adopting a named owned binding": "An unmarked `using var y := x;` is
# a compile error when its right-hand side is a named owned binding whose
# declared type, nullability stripped, is a class or interface type", and a
# named owned binding is, among others, "an `owned` parameter". "`using var y
# := move x;` transfers a named owned local or `owned` parameter into a
# scope-bound binding". §28.12 covers `await using var y := x;` the same way.
_OP_CALL = ['    Take(new Res());']
_x('owned_param', 'adopt_unmarked', ('REJ', '§28.10'), _OP_CALL,
   members='  public static method Take(owned src: Res)\n'
           '    using var x := src;\n'
           '  end Take;\n')
_x('owned_param', 'adopt_unmarked_await', ('REJ', ['§28.10', '§28.12']),
   ['    await Take(new Res());'],
   members='  public static async method Take(owned src: Res): Task\n'
           '    await using var x := src;\n'
           '  end Take;\n')
# The corrected spelling is the CONTROL: `move` adopts and `y` carries the
# obligation, so the destructor runs once, in the callee.
_x('owned_param', 'adopt_move', ('RUN', ['DTOR']), _OP_CALL,
   members='  public static method Take(owned src: Res)\n'
           '    using var x := move src;\n'
           '  end Take;\n')

# §28.12: "`await using` takes §28.10's admission unchanged", and a class that
# does not implement `IAsyncDisposable` takes the synchronous disposal.
_x('owned_param', 'adopt_move_await', ('RUN', ['DTOR']),
   ['    await Take(new Res());'],
   members='  public static async method Take(owned src: Res): Task\n'
           '    await using var x := move src;\n'
           '  end Take;\n')

# -- #7486 / #7415: an owned move-only parameter's `using var` copy ----------
# §28.10: "Where `x` is a local of move-only value type (§15.3) the unmarked
# form adopts: a move-only binding has no borrow reading to disambiguate, so
# the plain read moves" and "an unmarked read of a move-only value binding
# moves instead and is an owned right-hand side"; §15.3: the move-only kinds
# are the owning choice, "its nullable wrapper `C?` included (§35.9)". So the
# callee owns the payload, `x` disposes it once at scope end, and the caller's
# `Take(move src)` leaves nothing to dispose (the destructor runs ONCE).
_OC_CALL = ['    var src := OChoice.Text(new Res());', '    Take(move src);']
_x('owned_param_choice', 'using_var_param', ('RUN', ['DTOR']), _OC_CALL,
   members='  public static method Take(owned src: OChoice)\n'
           '    using var x := src;\n'
           '  end Take;\n')
_x('owned_param_nullable', 'using_var_param', ('RUN', ['DTOR']),
   ['    var src: OChoice? := OChoice.Text(new Res());', '    Take(move src);'],
   members='  public static method Take(owned src: OChoice?)\n'
           '    using var x := src;\n'
           '  end Take;\n')

# -- #7405 / #7406: the pin and the move-out ---------------------------------
# §28.10 "`using var` initialized from a borrow": the `using var` "still pins
# the borrow's source for the duration of the scope ... the consume-site
# freeze then governs every consume of the source in that range -- the frozen
# kills (a move, a transfer, a reassignment of the binding) are compile
# errors". And axiom A5: "`using var` is a scope pin: non-reassignable, never
# movable out" (§28.3: "it cannot be moved out of, only borrowed from"); the
# site-announced kill (`a.Dispose()`) stays permitted, but the later `move a`
# is still a move OUT of a `using var`.
_PIN_DECLS = ('public class RC\n'
              '  public constructor() end constructor;\n'
              '  destructor Console.WriteLine("DTOR"); end destructor;\n'
              '  public method Use() Console.WriteLine("U"); end Use;\n'
              '  public static method Take(owned r: RC)\n'
              '    using var k := move r;\n'
              '  end Take;\n'
              'end RC;\n')
_x('pin_move_out', 'pin_then_transfer', ('REJ', ['§28.10', '§28.3']),
   ['    var a := new RC();', '    var b := a;', '    using var p := b;',
    '    RC.Take(move a);', '    p.Use();'], decls=_PIN_DECLS)
# CONTROL: the same program minus the transfer. The pin is a borrow-pin, the
# source `a` keeps the obligation and disposes at scope end.
_x('pin_move_out', 'pin_no_transfer', ('RUN', ['U', 'DTOR']),
   ['    var a := new RC();', '    var b := a;', '    using var p := b;',
    '    p.Use();'], decls=_PIN_DECLS)
_x('pin_move_out', 'move_out_of_using_var', ('REJ', ['§28.3', '§28.10']),
   ['    using var a := new RC();', '    RC.Take(move a);'], decls=_PIN_DECLS)
_x('pin_move_out', 'move_out_after_dispose', ('REJ', ['§28.3', '§28.10']),
   ['    using var a := new RC();', '    a.Dispose();', '    RC.Take(move a);'],
   decls=_PIN_DECLS)
# CONTROL: a plain `var` owned local IS movable (§28.3.7).
_x('pin_move_out', 'move_out_of_var', ('RUN', ['DTOR']),
   ['    var a := new RC();', '    RC.Take(move a);'], decls=_PIN_DECLS)

# -- #3515: per-level reclamation --------------------------------------------
# §28.12 step 2: "Physical reclamation completes the disposal: owned fields
# are disposed in reverse declaration order and the base class's reclamation
# chains, once, immediately after logical disposal has completed, whichever
# path ran ... at each level of the reclamation chain the destructor body is
# skipped if that level declares or overrides a non-`abstract`
# `DisposeAsync`". So the derived level's OWNED FIELD must be disposed
# after `Der.DisposeAsync` even though the static type is the base.
_PL_DECLS = (
    'public class BaseV: IAsyncDisposable\n'
    '  public constructor() end constructor;\n'
    '  public async virtual method DisposeAsync(): Task\n'
    '    Console.WriteLine("BDA");\n'
    '  end DisposeAsync;\n'
    'end BaseV;\n'
    'public class DerV: BaseV\n'
    '  public owned _d: Res;\n'
    '  public constructor()\n'
    '    _d := new Res();\n'
    '  end constructor;\n'
    '  public async override method DisposeAsync(): Task\n'
    '    await Task.Delay(0);\n'
    '    Console.WriteLine("DA");\n'
    '  end DisposeAsync;\n'
    'end DerV;\n'
    'public class DerF: Base, IAsyncDisposable\n'
    '  public owned _d: Res;\n'
    '  public constructor()\n'
    '    _d := new Res();\n'
    '  end constructor;\n'
    '  public async method DisposeAsync(): Task\n'
    '    await Task.Delay(0);\n'
    '    Console.WriteLine("DA");\n'
    '  end DisposeAsync;\n'
    'end DerF;\n')
_x('per_level_reclaim', 'await_using_virtual_base',
   ('RUN', ['DA', 'DTOR']),
   ['    await using var u: BaseV := new DerV();'], decls=_PL_DECLS)
# the base level (`Base`) declares no DisposeAsync, so its destructor body
# runs (step 2's per-level rule) AFTER the derived level's owned field
# (the generated Dispose: destructor, owned fields in reverse, then base).
_x('per_level_reclaim', 'await_using_plain_base',
   ('RUN', ['DA', 'DTOR', 'BASE_DTOR']),
   ['    await using var u: Base := new DerF();'], decls=_PL_DECLS)
# §28.12 "If a class implements `IAsyncDisposable` but has no destructor,
# synchronous `Dispose()` performs only physical reclamation (owned-field
# disposal and base chaining)".
_x('per_level_reclaim', 'using_var_virtual_base', ('RUN', ['DTOR']),
   ['    using var u: BaseV := new DerV();'], decls=_PL_DECLS)

# -- #7463: an IOwningCollection<T> local ------------------------------------
# stdlib 4.2: "owning collections are themselves owned objects (their
# `Dispose()` cascades through their elements)"; and (stdlib 4.1) an owned
# value held at an interface type "is disposed at its discharge points --
# scope end, `using var`, the discard -- by dispatch to the runtime type
# (spec §17.13)". So the `using var` over the owned call result disposes the
# OwningArray and each element: the element's destructor runs once.
_x('owning_collection_local', 'using_var_interface', ('RUN', ['DTOR']),
   ['    using var c: IOwningCollection<Res> := Program.Make();'],
   members='  public static method Make(): owned IOwningCollection<Res>\n'
           '    var a := new OwningArray<Res>();\n'
           '    a.Add(new Res());\n'
           '    return a;\n'
           '  end Make;\n')
_x('owning_collection_local', 'using_var_concrete', ('RUN', ['DTOR']),
   ['    using var c := new OwningArray<Res>();', '    c.Add(new Res());'])

# -- the iterator-frame heads (#2623, #7422) ---------------------------------
# §29.7: "When a consumer stops iterating early -- `break`, exception, or
# `using var` ending before exhaustion -- the iterator is disposed", and the
# state machine "runs the pending cleanup actions active at the point of
# suspension -- `using var` disposals ... and `finally` clauses". The frame
# is advanced into the `try`, so disposal must print the `finally` line once.
# §28.5: an owned field's value is disposed by the owner's disposal.
_AE_GEN = ('  public static async method Gen(): IAsyncEnumerator<int>\n'
           '    try\n'
           '      yield return 1;\n'
           '      yield return 2;\n'
           '    finally\n'
           '      Console.WriteLine("FIN");\n'
           '    end;\n'
           '  end Gen;\n')
_x('async_enumerator', 'using_var', ('RUN', ['FIN']),
   ['    using var e := Program.Gen();', '    var more := await e.MoveNextAsync();'],
   members=_AE_GEN)
_x('async_enumerator', 'await_using_var', ('RUN', ['FIN']),
   ['    await using var e := Program.Gen();',
    '    var more := await e.MoveNextAsync();'], members=_AE_GEN)
_x('async_enumerator', 'field_store', ('RUN', ['FIN']),
   ['    using var h := new Holder();', '    h.F := Program.Gen();',
    '    var more := await h.F!.MoveNextAsync();'], members=_AE_GEN,
   decls='public class Holder\n'
         '  public owned F: IAsyncEnumerator<int>? := null;\n'
         '  public constructor() end constructor;\n'
         'end Holder;\n')
_x('sync_enumerator', 'field_store', ('RUN', ['FIN']),
   ['    using var h := new Holder();', '    h.F := Program.Gen();',
    '    var more := h.F!.MoveNext();'],
   members='  public static method Gen(): IEnumerator<int>\n'
           '    try\n'
           '      yield return 1;\n'
           '      yield return 2;\n'
           '    finally\n'
           '      Console.WriteLine("FIN");\n'
           '    end;\n'
           '  end Gen;\n',
   decls='public class Holder\n'
         '  public owned F: IEnumerator<int>? := null;\n'
         '  public constructor() end constructor;\n'
         'end Holder;\n')
# the enumerable is never advanced, so its `finally` never runs; the cell is
# the frame's reclamation (the loop leak oracle).
_x('async_enumerable', 'field_store', ('RUN', []),
   ['    using var h := new Holder();', '    h.F := Program.Gen();'],
   members='  public static async method Gen(): IAsyncEnumerable<int>\n'
           '    try\n'
           '      yield return 1;\n'
           '      yield return 2;\n'
           '    finally\n'
           '      Console.WriteLine("FIN");\n'
           '    end;\n'
           '  end Gen;\n',
   decls='public class Holder\n'
         '  public owned F: IAsyncEnumerable<int>? := null;\n'
         '  public constructor() end constructor;\n'
         'end Holder;\n')


def build_extra(head, form):
    """-> (source, expectation) for an EXTRA cell."""
    decls, members, setup, lines, expect = EXTRA[(head, form)]
    if expect[0] == 'REJ':
        body = list(lines)
    else:
        body = _loop_body(setup, lines)
    body.append('    Console.WriteLine("END");')
    return ('module ' + modname(head, form) + ';\n' + PRELUDE + decls +
            'public class Program\n' + members +
            '  public static async method Main(): Task\n' +
            '\n'.join(body) + '\n'
            '  end Main;\n'
            'end Program;\n'), expect


def build(head, form):
    """-> (source, expectation) for a cell, or None when the cell is not
    applicable (the form does not exist for that head)."""
    if (head, form) in EXTRA:
        return build_extra(head, form)
    if (head, form) not in EXPECT:
        return None
    if form == 'field_store' and head not in FIELD_HOLDER:
        return None
    label, ty, rhs, extra = HEADS[head][0], HEADS[head][1], HEADS[head][2], HEADS[head][3]
    setup = HEAD_SETUP.get(head, '')
    decls = PRELUDE + extra
    expect = EXPECT[(head, form)]
    if form == 'copy_param_source':
        if head in GENERIC_CLASS:
            return _generic_param_program(head), expect
        return _param_source_program(head, ty, rhs, extra, setup), expect
    if head in GENERIC_CALL:
        # The binding under test lives inside the monomorphized wrapper; the
        # program body is the call. The wrapper's method is async so that the
        # `await using var` form is admitted (§28.12's validity restriction).
        cls = GENERIC_CLASS[head]
        lines = _bind(ty, rhs, form)
        wrapper = (f'public class {cls}<T> where T: new()\n'
                   '  public static async method Run(): Task\n' +
                   '\n'.join(_wrap_loop(lines)) + '\n'
                   '    Console.WriteLine("body");\n'
                   '  end Run;\n'
                   f'end {cls};\n')
        decls += wrapper
        body = [GENERIC_CALL[head], '    Console.WriteLine("END");']
        src = ('module ' + modname(head, form) + ';\n' + GENERIC_DECL.get(head, '') + decls +
               'public class Program\n'
               '  public static async method Main(): Task\n' +
               '\n'.join(body) + '\n'
               '  end Main;\n'
               'end Program;\n')
        return src, expect
    body = []
    lines = _bind(ty, rhs, form)
    outside = setup if head in SETUP_OUTSIDE_LOOP else ''
    inside = '' if head in SETUP_OUTSIDE_LOOP else setup
    if outside:
        body.extend(outside.rstrip('\n').split('\n'))
    if expect is None or expect[0] != 'REJ':
        # Every cell that can make a leak claim loops, and the head's setup
        # lives inside the loop so that every iteration builds a fresh
        # right-hand side.
        body.extend(_loop_body(inside, lines))
    else:
        if inside:
            body.extend(inside.rstrip('\n').split('\n'))
        body.extend(lines)
    if head == 'task':
        # the Task head needs a TaskCompletionSource and a completed result
        body.append('    _ := tcs.TrySetResult(77);')
    body.append('    Console.WriteLine("END");')
    if form == 'field_store':
        decls += FIELD_HOLDER.get(head, '')
    src = ('module ' + modname(head, form) + ';\n' + decls +
           'public class Program\n'
           '  public static async method Main(): Task\n' +
           '\n'.join(body) + '\n'
           '  end Main;\n'
           'end Program;\n')
    return src, expect


def build_twin(head):
    """The plain-`var` twin of a head's binding: the same setup, the same
    right-hand side, but the plain `var` spelling. §28.10 makes the `using var`
    spelling's RUNTIME LIFETIME the same as this twin's, so a cell's
    "must not leak" claim is equivalent to "same lifetime as this twin" when
    the twin is CLEAN. The twin's class is measured per head by `--twins`."""
    label, ty, rhs, extra = HEADS[head][0], HEADS[head][1], HEADS[head][2], HEADS[head][3]
    if head in GENERIC_CLASS:
        cls = GENERIC_CLASS[head]
        decls = PRELUDE
        decls += (f'public class {cls}<T> where T: new()\n'
                  '  public static async method Run(): Task\n'
                  f'    var x := new T();\n'
                  '    Console.WriteLine("body");\n'
                  '  end Run;\n'
                  f'end {cls};\n')
        body = [GENERIC_CALL[head], '    Console.WriteLine("END");']
        return ('module ' + modname(head, 'twin') + ';\n' +
                GENERIC_DECL.get(head, '') + decls +
                'public class Program\n'
                '  public static async method Main(): Task\n' +
                '\n'.join(body) + '\n'
                '  end Main;\n'
                'end Program;\n')
    setup = HEAD_SETUP.get(head, '')
    ann = f': {ty}' if ty else ''
    body = []
    outside = setup if head in SETUP_OUTSIDE_LOOP else ''
    inside = '' if head in SETUP_OUTSIDE_LOOP else setup
    if outside:
        body.extend(outside.rstrip('\n').split('\n'))
    body.extend(_loop_body(inside, [f'    var x{ann} := {rhs};']))
    if head == 'task':
        body.append('    _ := tcs.TrySetResult(77);')
    body.append('    Console.WriteLine("END");')
    return ('module ' + modname(head, 'twin') + ';\n' + PRELUDE + extra +
            'public class Program\n'
            '  public static async method Main(): Task\n' +
            '\n'.join(body) + '\n'
            '  end Main;\n'
            'end Program;\n')


def all_cells(filt=None):
    toks = [t for t in (filt or '').split(',') if t]
    out = []
    for h in HEADS:
        for f, flabel in FORMS:
            # The form's NAME is matchable, not only its label: the record
            # documented `--filter copy_local_source`, and the filter did not
            # match it (exit 2, "no cells match the filter").
            if toks and not any(t in f'{h} {f} {flabel} {HEADS[h][0]}'
                                for t in toks):
                continue
            out.append((h, f))
    # The EXTENDED cells (see `EXTRA`), in declaration order.
    for (h, f) in EXTRA:
        if toks and not any(t in f'{h} {f} {EXTRA_HEADS[h]}' for t in toks):
            continue
        out.append((h, f))
    return out


# ---------------------------------------------------------------------------
# Runner
# ---------------------------------------------------------------------------

LEAK_RE = re.compile(r'(\d+) byte\(s\) leaked in (\d+) allocation')
GLIBC_RE = re.compile(r'free\(\): |double free|invalid pointer|corrupted')
# The §28.9 VACUITY lint, recognised by the RULE it names and not by the
# lint's wording: the lint says "`using var` `x` is vacuous — … (§28.9) …",
# and it is a WARNING. A needle on the wording alone ("is vacuous") is an
# implementation string, and it breaks the moment the message is reworded —
# which the house calls a behaviour flip. The § marker is the rule, and the
# compiler cites its rule in every diagnostic.
VACUOUS_RE = re.compile(r'warning:.*§28\.9')
DIAG_RE = re.compile(r'warning:|error:')


def needle_hit(log, needle):
    """True iff `log` holds the cell's needle.

    A needle is a string or a LIST of acceptable markers; ANY one of them
    suffices. A list is how a cell names a rule that more than one section
    can decide (`['§11.3', '§12.1', '§28.12']` for a member that is not
    declared), and it is how a cell stays green when the SAME rule is cited
    under a different section after a fix.

    What a needle may NOT be: a generic token that a NEIGHBOURING rule's
    diagnostic also carries. "using var" is the compiler's §28.3 must-use
    remedy, so a cell that named it was green on §28.3's rule while claiming
    §28.10's — and it went RED when a lane implemented §28.10 properly (the
    §28.10 diagnostic's remedies are `move x` and a `var`/`const` binding,
    not "using var"). Every REJ needle therefore names a §.
    """
    if isinstance(needle, (list, tuple)):
        return any(n in log for n in needle)
    return needle in log


def flatten(text):
    """One line, no tabs: the report TSV's detail column must not split a row
    across physical lines (12 of the 153 rows did, before this)."""
    return (text or '').replace('\\', '\\\\').replace('\t', ' ').replace(
        '\r', ' ').replace('\n', ' | ')


def classify(compiled, rc, err):
    if not compiled:
        return CF, 'compile failed'
    m = re.search(r'ERROR: AddressSanitizer: ([A-Za-z_-]+)', err)
    if m:
        return CRASH, m.group(1)
    if GLIBC_RE.search(err):
        return CRASH, 'glibc'
    if 'ERROR: LeakSanitizer' in err:
        lm = LEAK_RE.search(err)
        b, a = (lm.group(1), lm.group(2)) if lm else ('?', '?')
        return LEAK, f'{b}B/{a}'
    if rc != 0:
        return CRASH, f'rc={rc}'
    return CLEAN, ''


def compile_program(bezelc, odir, progdir, name, src, timeout):
    """Compile one program. -> ('compiled'|'rejected'|'timeout', log)."""
    prog = f'{progdir}/{name}.bzl'
    with open(prog, 'w') as f:
        f.write(src)
    exe = f'{odir}/{name}.exe'
    # A stale executable from an earlier run in the same `--out` directory must
    # not be mistaken for THIS run's: the sanitizer counter reads it.
    if os.path.exists(exe):
        os.remove(exe)
    cmd = [bezelc, '--sanitize=address', '-o', exe, prog]
    try:
        p = subprocess.run(cmd, capture_output=True, text=True,
                           timeout=timeout, errors='replace')
    except subprocess.TimeoutExpired:
        return 'timeout', 'compile timeout'
    clog = p.stdout + p.stderr
    with open(f'{odir}/{name}.clog', 'w') as f:
        f.write(clog)
    if p.returncode != 0:
        return 'rejected', clog
    return 'compiled', clog


def run_exe(exe, odir, name, run_timeout):
    """Run a compiled program. -> ('ran'|'timeout', class, detail, stdout)."""
    try:
        r = subprocess.run([exe], capture_output=True, text=True,
                           timeout=run_timeout, errors='replace')
    except subprocess.TimeoutExpired:
        return 'timeout', TIMEOUT, 'run timeout', []
    with open(f'{odir}/{name}.out', 'w') as f:
        f.write(r.stdout)
    with open(f'{odir}/{name}.err', 'w') as f:
        f.write(r.stderr)
    cls, det = classify(True, r.returncode, r.stderr)
    lines = [ln for ln in r.stdout.splitlines() if ln.strip()]
    return 'ran', cls, det, lines


def asan_instrumented(exe):
    """True iff the built executable carries the ASan runtime.

    The grid's leak/crash verdicts are only sanitizer verdicts if the binary
    it runs is ASan-instrumented. The house's
    `NATIVE SANITIZER: n check(s) skipped` counts the cells whose check
    could not run because the ASan RUNTIME was unavailable, and this is the
    source of that number: an executable with no ASan runtime in it is a
    cell whose sanitizer check was skipped.
    """
    try:
        with open(exe, 'rb') as f:
            blob = f.read()
    except OSError:
        return False
    return b'__asan' in blob or b'libclang_rt.asan' in blob


def asan_skipped_count(odir, cells):
    """The number of cells whose ASan check was SKIPPED.

    THE COUNTER. A cell's sanitizer check was skipped when the cell
    produced an executable that carries NO ASan runtime: the runtime was
    unavailable, so the cell's leak/crash verdict is not a sanitizer
    verdict.

    A cell that produced NO executable is NOT counted: a program rejected
    at compile time has no sanitizer verdict to skip, and a cell that was
    expected to run and did not build is a WRONG cell (or a TIMEOUT cell),
    which blocks the DONE verdict on its own. Nothing is hidden by this
    counter — an unmeasured cell blocks the verdict.
    """
    n = 0
    for head, form in cells:
        exe = os.path.join(odir, f'{head}__{form}.exe')
        if os.path.exists(exe) and not asan_instrumented(exe):
            n += 1
    return n


def run_cell(bezelc, odir, progdir, head, form, timeout, run_timeout, twins):
    """One cell. -> (head, form, verdict, observed, detail).

    The detail column carries a CELL-RELATIVE program path
    (`programs/<cell>.bzl`), so two runs into different `--out`
    directories produce byte-identical report TSVs.
    """
    head_, form_, verdict, observed, detail = _run_cell(
        bezelc, odir, progdir, head, form, timeout, run_timeout, twins)
    name = f'{head}__{form}'
    absolute = os.path.join(progdir, name + '.bzl')
    relative = os.path.join('programs', name + '.bzl')
    if isinstance(detail, (list, tuple)):
        detail = ' '.join(str(x) for x in detail)
    detail = (detail or '').replace(absolute, relative)
    observed = (observed or '').replace(absolute, relative)
    return head_, form_, verdict, observed, detail


def _run_cell(bezelc, odir, progdir, head, form, timeout, run_timeout, twins):
    """One cell, before the path rewrite. -> (head, form, verdict, observed, detail)."""
    built = build(head, form)
    if built is None:
        return head, form, 'N/A', 'not applicable', ''
    src, expect = built
    name = f'{head}__{form}'
    status, clog = compile_program(bezelc, odir, progdir, name, src, timeout)
    if status == 'timeout':
        return head, form, 'TIMEOUT', 'compile timeout', ''
    if expect is None:
        # The spec does not decide this cell: RUN it and report what happens,
        # so the ruling can be made against an observation.
        if status != 'compiled':
            first = next((ln for ln in clog.splitlines()
                          if ln.strip() and 'warning' not in ln), '')
            return head, form, 'UNDECIDED', f'rejected: {first[:120]}', ''
        st, cls, det, lines = run_exe(f'{odir}/{name}.exe', odir, name, run_timeout)
        if st != 'ran':
            return head, form, 'UNDECIDED', 'run timeout (hang)', ''
        return head, form, 'UNDECIDED', f'{cls} {det} output {lines[:6]}', ''
    if expect[0] == 'REJ':
        if status == 'compiled':
            first = next((ln for ln in clog.splitlines()
                          if ln.strip() and 'warning' not in ln), '')
            return head, form, 'WRONG', 'compiled, expected rejection', first
        needle = expect[1]
        # The needle must be found in the diagnostic that REJECTED the
        # program — the non-warning lines — and not anywhere in the log: a
        # warning that happens to cite the section (the §28.10 borrow-pin
        # note does) would otherwise make the cell green on a diagnostic that
        # did not decide it.
        rejecting = '\n'.join(ln for ln in clog.splitlines()
                              if ln.strip() and 'warning:' not in ln)
        if not needle_hit(rejecting, needle):
            first = next((ln for ln in clog.splitlines()
                          if ln.strip() and 'warning' not in ln), '')
            return head, form, 'WRONG', (
                f'rejected, but no {needle!r} in the diagnostic'), first
        return head, form, 'OK', f'rejected ({needle})', ''
    if status != 'compiled':
        first = next((ln for ln in clog.splitlines()
                      if ln.strip() and 'warning' not in ln), '')
        return head, form, 'WRONG', 'rejected, expected a run', first
    st, cls, det, lines = run_exe(f'{odir}/{name}.exe', odir, name, run_timeout)
    if st != 'ran':
        return head, form, 'WRONG', 'run timeout (hang)', ''
    if cls == CRASH:
        return head, form, 'WRONG', f'{cls}: {det}', lines[:4]
    if expect[0] == 'TWIN':
        # §28.10: the `using var` spelling's RUNTIME LIFETIME must be the
        # plain-`var` twin's. The twin's class is the reference.
        tcls, tdet = twins.get(head, ('?', '?'))
        if cls != tcls:
            return head, form, 'WRONG', (
                f'leak {cls}:{det} != the plain-var twin\'s {tcls}:{tdet}'), ''
    elif cls != CLEAN:
        return head, form, 'WRONG', f'{cls}: {det}', lines[:4]
    if 'END' not in lines:
        return head, form, 'WRONG', 'the program did not reach its END marker', lines[:4]
    lines = [ln for ln in lines if ln != 'END']
    want = list(expect[1])
    tail = list(expect[2]) if len(expect) > 2 else []
    # The loop prints the per-iteration markers LOOP_N times; the tail lines
    # come after the loop.
    want_full = want * LOOP_N + tail
    if lines != want_full:
        got = lines[:6]
        return head, form, 'WRONG', f'output {got} != expected {want_full[:6]}', ''
    if expect[0] == 'VAC' and not VACUOUS_RE.search(clog):
        return head, form, 'WRONG', 'no §28.9 vacuity warning in the compile log', ''
    if expect[0] == 'NOVAC' and VACUOUS_RE.search(clog):
        return head, form, 'WRONG', 'a §28.9 vacuity warning was emitted where none is due', ''
    return head, form, 'OK', f'clean, output {want}', ''


def measure_twins(bezelc, odir, progdir, timeout, run_timeout):
    """Measure each head's plain-`var` twin. The twin's class is the reference
    §28.10's "the only effect is the reassignability restriction" is measured
    against: a cell's "must not leak" claim is equivalent to "same lifetime as
    the twin" exactly when the twin is CLEAN. The twins are also a finding in
    their own right: a twin that LEAKS is an `Object`-slot retention the
    `using var` spelling inherits."""
    out = {}
    for h in HEADS:
        name = f'{h}__twin'
        status, clog = compile_program(bezelc, odir, progdir, name,
                                       build_twin(h), timeout)
        if status != 'compiled':
            out[h] = ('CF', 'compile failed')
            continue
        st, cls, det, lines = run_exe(f'{odir}/{name}.exe', odir, name, run_timeout)
        if st != 'ran':
            out[h] = ('TIMEOUT', 'run timeout')
            continue
        out[h] = (cls, det)
    return out


# ---------------------------------------------------------------------------
# Judging
# ---------------------------------------------------------------------------


def _row(rows, head, form):
    for r in rows:
        if r[0] == head and r[1] == form:
            return r
    return None


def admission(verdict, observed, expect):
    """Did the program COMPILE? True = admitted, False = rejected, None = the
    cell says nothing about the admission.

    The spelling-agreement check needs this: §28.12's "`await using var` takes
    §28.10's admission UNCHANGED" is a claim about the ADMISSION, not about
    the runtime behaviour, so the two spellings are compared on it.
    """
    if observed.startswith('rejected'):
        return False
    if observed.startswith('compiled'):
        return True
    if expect is not None and expect[0] != 'REJ' and verdict in ('OK', 'WRONG'):
        # A RUN-family cell that is not OK still COMPILED (it crashed, leaked,
        # printed the wrong output, or hung at run time) — the program was
        # admitted either way.
        return True
    return None


def check_agreement(rows, expectations=None):
    """§28.12's "takes §28.10's admission UNCHANGED" — the two spellings of the
    same program must AGREE on the admission.

    Returns a list of (head, form, verdict, observed) overrides. A head in
    OPEN_QUESTION is NOT judged: the spec question is the OWNER's, and the
    grid must not settle it by counting the cell wrong (the record keeps the
    question open, and the held-out hunk with it).
    """
    if expectations is None:
        expectations = EXPECT
    out = []
    for head in list(HEADS) + list(EXTRA_HEADS):
        for plain_form, await_form in AGREEMENT_PAIRS:
            p = _row(rows, head, plain_form)
            a = _row(rows, head, await_form)
            if p is None or a is None:
                continue
            pa = admission(p[2], p[3], expectations.get((head, plain_form)))
            aa = admission(a[2], a[3], expectations.get((head, await_form)))
            if pa is None or aa is None or pa == aa:
                continue
            if head in OPEN_QUESTION:
                # The OWNER's question — the grid adds no verdict of its own.
                continue
            out.append((head, await_form, 'MISMATCH',
                        f'the two spellings disagree: {plain_form} is '
                        f'{"admitted" if pa else "rejected"} and '
                        f'{await_form} is '
                        f'{"admitted" if aa else "rejected"} (§28.12 '
                        "takes §28.10's admission unchanged)"))
    return out


def blocking_count(jc):
    """The number of cells that BLOCK the DONE verdict.

    THE DONE CRITERION. "No WRONG cell, no KNOWN-RED cell, no SPELLING
    MISMATCH and no TIMEOUT cell" — a red cell is red, whoever owns it, and a
    cell whose program hung the compiler is UNMEASURED, so it must block too:
    a DONE instrument must not print YES with an unmeasured cell. The
    reviewer's demonstration: with the KNOWN-RED demotion and no tracker
    check, a run printed "5 known-red" and EXITED 0 with five cells red and
    leaking.

    THE QUARANTINE. A quarantined cell (the `task` row, pending #6194) is
    counted separately and never moves the DONE verdict: a row that flips
    run-to-run cannot be part of a verdict that must not flip.
    """
    return (jc.get('WRONG', 0) + jc.get('KNOWN-RED', 0) +
            jc.get('MISMATCH', 0) + jc.get('TIMEOUT', 0))


def foreign_rows(rows):
    """The rows judged FOREIGN(#N), in grid order."""
    return [r for r in rows if r[2].startswith('FOREIGN(')]


def summary_lines(total, jc, asan_skipped, quarantined, quarantined_verdicts,
                  foreign=None):
    """The summary the grid prints, as LINES (so a test can assert on them).

    `asan_skipped` is the count of cells whose ASan check was skipped
    because the ASan RUNTIME was unavailable — its OWN counter (see
    `asan_skipped_count`), never the timeout count. A TIMEOUT is reported in
    the cells line as a TIMEOUT and it BLOCKS the verdict.

    `foreign` is the list of FOREIGN rows (see `FOREIGN`): shown on their own
    line, with the issue that owns each, and never counted in the DONE number.
    """
    foreign = foreign or []
    lines = [
        f'{total} cells: {jc.get("OK", 0)} ok, {jc.get("WRONG", 0)} wrong, '
        f'{jc.get("KNOWN-RED", 0)} known-red, {jc.get("MISMATCH", 0)} '
        f'mismatch, {jc.get("UNDECIDED", 0)} undecided, '
        f'{jc.get("N/A", 0)} n/a, {jc.get("TIMEOUT", 0)} timeout',
    ]
    if quarantined:
        lines.append(
            f'QUARANTINED: {len(quarantined)} cell(s) held out of the DONE '
            f'count (pending {sorted(set(QUARANTINE.values()))[0]}): '
            + ', '.join(f'{v} {k}' for k, v in
                        sorted(quarantined_verdicts.items())))
    if foreign:
        per_issue = {}
        for r in foreign:
            tag = r[2][len('FOREIGN('):-1]
            per_issue[tag] = per_issue.get(tag, 0) + 1
        lines.append(
            f'FOREIGN: {len(foreign)} cell(s) shown but held out of the DONE '
            'count (another campaign owns the defect): '
            + ', '.join(f'{k} x{v}' for k, v in sorted(per_issue.items())))
    lines.append(f'NATIVE SANITIZER: {asan_skipped} check(s) skipped')
    blocking = blocking_count(jc)
    if blocking:
        lines.append(f'DONE: NO — {blocking} cell(s) block it (wrong + '
                     'known-red + mismatch + timeout)')
    elif foreign:
        lines.append('DONE: YES — no cell blocks the verdict '
                     f'({len(foreign)} FOREIGN cell(s) are outside the count)')
    else:
        lines.append('DONE: YES — no cell blocks the verdict')
    return lines


def judge(results, check_issues=False):
    """The cells -> the counts, the DONE verdict, and the rows.

    THE DONE CRITERION is `blocking_count`: no WRONG cell, no KNOWN-RED cell,
    no SPELLING MISMATCH and no TIMEOUT cell.

    THE QUARANTINE. A quarantined cell (the `task` row, pending #6194) is
    counted separately and never moves the DONE verdict: a row that flips
    run-to-run cannot be part of a verdict that must not flip.
    """
    overrides = dict(((h, f), (v, o)) for h, f, v, o in
                     check_agreement(results))
    rows = []
    quarantined_verdicts = {}
    for head, form, verdict, observed, detail in results:
        if (head, form) in overrides:
            v, o = overrides[(head, form)]
            # The mismatch is the cell's HEADLINE verdict — the grid's whole
            # point is to see the disagreement — and the cell's own reason
            # stays in the text.
            verdict = v
            observed = f'{o} [cell: {observed}]'
        if verdict == 'WRONG':
            known = KNOWN_RED.get((head, form))
            if known and (known[1] in observed or known[1] in detail):
                if check_issues and not issue_is_open(known[0]):
                    observed = (f'{observed} [{known[0]} — the issue is CLOSED, '
                                'so this is not a KNOWN-RED]')
                else:
                    verdict = 'KNOWN-RED'
                    observed = f'{observed} [{known[0]}, fixed by {known[2]}]'
        if (head, form) in QUARANTINE:
            quarantined_verdicts[verdict] = quarantined_verdicts.get(
                verdict, 0) + 1
            verdict = 'QUARANTINED'
        elif (head, form) in FOREIGN:
            issue = FOREIGN[(head, form)]
            if verdict in ('WRONG', 'MISMATCH', 'KNOWN-RED'):
                if check_issues and not issue_is_open(issue):
                    observed = (f'{observed} [{issue} - the issue is CLOSED, '
                                'so this is not FOREIGN]')
                else:
                    observed = (f'{verdict}: {observed} [{issue}, owned by '
                                'another campaign]')
                    verdict = f'FOREIGN({issue})'
            elif verdict == 'OK':
                observed = (f'{observed} [{issue} no longer explains a red '
                            'here: delete the FOREIGN row]')
        rows.append((head, form, verdict, observed, detail))
    counts = {}
    for r in rows:
        counts[r[2]] = counts.get(r[2], 0) + 1
    quarantined = [r for r in rows if r[2] == 'QUARANTINED']
    judged = [r for r in rows
              if r[2] != 'QUARANTINED' and not r[2].startswith('FOREIGN(')]
    jc = {}
    for r in judged:
        jc[r[2]] = jc.get(r[2], 0) + 1
    blocking = blocking_count(jc)
    done = blocking == 0
    return rows, counts, jc, quarantined, done, quarantined_verdicts


def issue_is_open(number):
    """Is the issue OPEN? A live check, used only with `--check-issues`: the
    grid itself needs no network, so the tracker is consulted only when the
    caller asks for it. An issue that cannot be reached counts as OPEN (the
    grid must not promote a KNOWN-RED to WRONG because the network is down).
    """
    tag = number.lstrip('#')
    try:
        p = subprocess.run(['gh', 'issue', 'view', tag, '--json', 'state'],
                           capture_output=True, text=True, timeout=30)
    except (OSError, subprocess.TimeoutExpired):
        return True
    if p.returncode != 0:
        return True
    try:
        return '"OPEN"' in p.stdout or 'OPEN' in p.stdout
    except Exception:
        return True


def write_report(rows, path):
    """The report TSV: ONE row per cell, FIVE fields per row, no embedded
    newline or tab, and a CELL-RELATIVE program path.

    The absolute path is what made two runs into different `--out` directories
    differ byte-for-byte; the multi-line detail is what split 12 of the 153
    rows across physical lines (166 lines for 154 expected, 12 rows with the
    wrong field count and one empty).
    """
    with open(path, 'w') as f:
        f.write('head\tform\tverdict\tobserved\tdetail\n')
        for head, form, verdict, observed, detail in rows:
            f.write('\t'.join((head, form, verdict, flatten(observed),
                               flatten(detail))) + '\n')


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--bezelc', default='build/bezelc')
    ap.add_argument('--out', default='/var/tmp/bezel-scratch/using-heads-grid')
    ap.add_argument('--filter', default='')
    ap.add_argument('--jobs', type=int, default=8)
    ap.add_argument('--timeout', type=int, default=60)
    ap.add_argument('--run-timeout', type=int, default=120)
    ap.add_argument('--report', default='')
    ap.add_argument('--twins', action='store_true',
                    help="also measure each head's plain-var twin")
    ap.add_argument('--check-issues', action='store_true',
                    help='consult the tracker: a KNOWN-RED whose issue is '
                         'CLOSED is not a KNOWN-RED (needs the network; the '
                         'grid itself never needs it)')
    args = ap.parse_args()

    bezelc = os.path.abspath(args.bezelc)
    if not os.path.exists(bezelc):
        print(f'using_heads_grid: no compiler at {bezelc}', file=sys.stderr)
        return 2
    cells = all_cells(args.filter)
    if not cells:
        print('using_heads_grid: no cells match the filter', file=sys.stderr)
        return 2
    odir = os.path.join(args.out, 'run')
    progdir = os.path.join(args.out, 'programs')
    os.makedirs(odir, exist_ok=True)
    os.makedirs(progdir, exist_ok=True)

    twins = measure_twins(bezelc, odir, progdir, args.timeout, args.run_timeout)
    results = []
    with concurrent.futures.ThreadPoolExecutor(max_workers=args.jobs) as ex:
        futs = [ex.submit(run_cell, bezelc, odir, progdir, h, f,
                          args.timeout, args.run_timeout, twins) for h, f in cells]
        for fut in futs:
            results.append(fut.result())
    order = {c: i for i, c in enumerate(cells)}
    results.sort(key=lambda r: order[(r[0], r[1])])

    (rows, counts, jc, quarantined, done,
     quarantined_verdicts) = judge(results,
                                  check_issues=args.check_issues)
    for head, form, verdict, observed, detail in rows:
        print(f'{verdict:12s} {head:20s} {form:22s} {observed}')
    total = len(rows)
    asan_skipped = asan_skipped_count(odir, cells)
    for line in summary_lines(total, jc, asan_skipped, quarantined,
                              quarantined_verdicts,
                              foreign=foreign_rows(rows)):
        print(line)

    if args.twins:
        print('plain-var twins:')
        for h in HEADS:
            cls, det = twins.get(h, ('?', '?'))
            print(f'  {h:24s} {cls} {det}')

    if args.report:
        write_report(rows, args.report)
        print(f'report written to {args.report}')
    return 0 if done else 1


if __name__ == '__main__':
    sys.exit(main())
