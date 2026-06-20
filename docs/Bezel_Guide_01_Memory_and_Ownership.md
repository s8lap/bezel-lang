# Bezel Guide 1: Memory and Ownership

*A short, example-first guide. Companion to the Bezel Language Specification v0.135.*

Bezel has no garbage collector. Every heap object has exactly one owner responsible for disposing it. The compiler tracks ownership and refuses to compile a program that would leak or double-free. This guide shows the everyday patterns; it does not restate the full rules (see spec §28).

The whole model reduces to one question for every class-typed value: **am I responsible for disposing this, or is someone else?** Owned means yes. Borrowed means no.

---

## 1. The two declarations you will actually type

```bezel
using var conn := new Connection("db://localhost");  // owned, disposed at block end
var view := conn.View();                             // borrowed, disposes nothing
```

`using var` is the workhorse. It says *I own this, dispose it when this block ends.* Plain `var` takes whatever ownership the right-hand side produced — here `View()` returns a borrow, so `view` is a borrow and carries no obligation.

You never write `owned` on a local. Locals get their state from how they are introduced; `owned` is for fields, parameters, and return types (sections 4–6).

The disposal is automatic and ordered. At the end of the enclosing block, `using var` values are disposed in reverse order of declaration, including on the exception path:

```bezel
method Import(path: string)
  using var file := new FileStream(path, FileMode.Open);  // disposed second
  using var parser := new Parser(file);    // disposed first, even if ReadAll throws
  parser.ReadAll();
end Import;
```

You do not write cleanup code. If a constructor or method between the two `using var` lines throws, both are still disposed, innermost first.

---

## 2. Borrowing: the default for "just let me use it"

A method that only *uses* an object should borrow it, not own it. Borrowing is the absence of `owned`:

```bezel
method PrintSummary(account: BankAccount)        // borrows account
  Console.WriteLine(account.Balance.ToString());
end PrintSummary;                                // disposes nothing
```

The caller keeps ownership; `PrintSummary` cannot dispose `account`, cannot store it past the call, and cannot return it. This is the common case — most methods borrow.

A borrow is valid only while its *source* stays alive. The compiler enforces this; you cannot return a borrow that outlives what it points into:

```bezel
method Leak(): View
  using var c := new Connection("...");
  return c.View();        // ERROR: borrow of c, but c is disposed at block end
end Leak;
```

The fix is to return an owned value instead (section 5), or to take the source as a borrowed parameter so the caller owns it.

---

## 3. How an expression's ownership is decided

You rarely think about this explicitly, but it is worth seeing once. The right-hand side of an assignment has an ownership state determined by its form:

```bezel
using var a := new Buffer();          // new T(...)            -> owned
var b := source.OpenBuffer();         // returns owned Buffer   -> owned
var c := source.Buffer;               // field / property read  -> borrowed (source: source)
var d := registry.Find("key");        // returns Buffer (no owned) -> borrowed
var e := move holder.slot;            // move out of owned field -> owned
```

The rule of thumb: `new` and any method whose return type says `owned` give you an owned value (you must discharge it). A plain field read or a plain (non-`owned`) return is a borrow (you must not). When in doubt, the spec's producers table (§28.3) is exhaustive.

---

## 4. Owning fields: objects that hold other objects

A field marked `owned` belongs to its containing object exclusively and is auto-disposed with it:

```bezel
class BankAccount(owner: Customer)
  private owned _transactions: Array<Transaction> := new Array<Transaction>();
  private owned _cache: TransactionCache? := null;   // nullable owned
  private _owner: Customer := owner;                  // back-reference, NOT owned
end BankAccount;
```

Three things to copy from this:

- **Resources you create and control are `owned`.** `_transactions` is disposed automatically when the `BankAccount` is disposed — no destructor needed.
- **Back-references are not `owned`.** `_owner` points *up* to something that outlives this object; this object did not create it and must not dispose it. Marking it `owned` would be a double-free waiting to happen.
- **A class whose only resources are `owned` fields needs no destructor.** The compiler-generated cleanup disposes them in reverse declaration order (nullable ones checked first). Write a destructor only for native/foreign resources.

`owned` is only for class- or interface-typed fields. `string` and function-typed fields are runtime-managed and cannot be `owned` (the compiler will tell you).

---

## 5. Returning ownership: factory and transfer methods

If a method *creates* something the caller must own, say so with an `owned` return type:

```bezel
method CloneBuffer(): owned Buffer
  var fresh := new Buffer();   // owned, because `new` produces an owned value
  fresh.CopyFrom(_source);
  return fresh;                // the owned value reaches the `owned` return — done
end CloneBuffer;
```

Note this is plain `var`, not `using var`. Ownership is tracked on the value no matter how the local is declared — `new` produces an owned value, and the rule is simply that an owned value must reach *one of* an `owned` field, a `using var`, an `owned` parameter, or an **`owned` return** before the method ends (or be explicitly disposed). `return fresh` under the `owned` return type *is* one of those owned locations, so the obligation is discharged by the return itself. Nothing is disposed here; ownership transfers to the caller, who writes:

```bezel
using var s := factory.CloneBuffer();   // caller now owns it, disposes it
```

**When would you use `using var` here instead?** Only for exception safety. The plain-`var` form above is correct *only if nothing between `new` and `return` can throw* — if `CopyFrom` could throw, `fresh` would be a `new`-result that never reached an owned location on the throwing path, which the compiler rejects as a leak. Writing `using var fresh := new Buffer();` fixes that: `using var` disposes its value *only on paths where ownership is still held at scope exit*. On the normal path, `return fresh` moves ownership out (it reaches the `owned` return), so there is nothing left for `using var` to dispose — it does **not** double-dispose. On the exception path, ownership never transferred, so `using var` disposes `fresh` during unwinding. `using var` means "dispose if still owned here," not "dispose unconditionally at the end." Use plain `var` when the body between `new` and `return` cannot throw; use `using var` when it can.

This is the one legal way to get an owned value past a method boundary. The compiler enforces the symmetry: an `owned` field cannot be returned through a non-`owned` return type (that would be a silent leak of the container's resource):

```bezel
method GetTransactions(): Array<Transaction>
  return _transactions;     // ERROR: cannot leak owned field through non-owned return
end GetTransactions;
```

If a caller needs to look at `_transactions` without taking it, return a borrow (non-`owned` return type) or expose a borrowing view; if a caller must take it, use `move` (next section).

---

## 6. `move`: handing an owned field's contents away

To transfer an owned field *out* of its container, use `move`. Because `move` leaves the field in moved-from state, the **field-restoration rule** (§28.8.1) requires that every path to method exit puts something back. For a *nullable* owned field the idiomatic pattern is to null the field after the move — `null` is a valid restoration for a `Logfile?`:

```bezel
// Field declared nullable so that null is a valid post-move restoration.
// private owned _log: Logfile?;
method DetachLog(): owned Logfile?
  var d := move _log;   // _log is now in moved-from state
  _log := null;         // restore the field — required by §28.8.1
  return d;
end DetachLog;
```

`move _log` reads `_log` as an owned value and leaves the field moved-from. The **field-restoration rule** applies: if you `move` out of a field, every path to the end of that method must put something back:

```bezel
method Rotate()
  using var old := move _log;          // moved out
  _log := new Logfile(NextName());     // restored before method exits — required
  old.Flush();
end Rotate;                            // old disposed here
```

Forgetting to reassign `_log` is a compile error, not a runtime surprise. The same `move` form works on owning-collection slots (`move buffer[i]`); see Guide 4.

---

## 7. The one rule that will actually trip you: branch joins

This is the most common compile error newcomers hit. If two branches of an `if` leave the same variable in *different* ownership states, the merge is rejected — because one path has a disposal obligation the other does not, and silently dropping it would leak:

```bezel
var x: Buffer;
if useFresh then
  x := new Buffer();              // x is OWNED here
else
  x := registry.Buffer;           // x is BORROWED here
end;
// ERROR at the merge: x is owned on one branch, borrowed on the other
```

The compiler will not guess. You equalize the branches yourself, one of two ways.

**Pattern A — make both branches borrow a source that outlives the `if`.** The goal: `target` is a borrow on both branches, and each branch's borrow source lives past the merge. Here is a realistic method. A writer sends bytes either to a caller-supplied stream or, if none is given, to its own file-backed stream held as a field. Whichever is chosen must still be valid *after* the `if`, where the bytes are actually written.

```bezel
class ResponseWriter
  // Our own file-backed stream, owned for the lifetime of this writer.
  private owned _fallback: FileStream :=
      new FileStream("responses.log", FileMode.Append);

  // `dest` is a borrowed parameter (no `owned`) — the caller owns it and
  // guarantees it outlives this call. `_fallback` is a field — it lives as
  // long as this ResponseWriter. Both outlive the `if` below.
  public method Write(dest: Stream?, payload: Array<byte>)
    // Both `dest` and `_fallback` are borrows (this method owns neither),
    // so they share ownership and `??` is well-formed. `??` is rejected
    // only when its two sides disagree on ownership (see the note after
    // this section on why `dest ?? new FileStream(...)` does not compile).
    var target: Stream := dest ?? _fallback;   // borrows whichever is chosen
    target.Write(payload, 0, payload.Length);
    target.Flush();
  end Write;
end ResponseWriter;
```

`dest ?? _fallback` reads: use `dest` if the caller supplied one, otherwise our `_fallback` field. Both are borrows, so the result is a borrow, and `target`'s source is whichever of the two long-lived objects was selected — both outlive the method, so the post-merge use is sound. This is the same code as the explicit form below, written the way you would reach for in most languages; here it happens to be legal because the ownership matches.

The explicit `if` is equivalent and is what you write when the per-branch logic needs more than a single selection, or when you want the ownership of each branch documented in a comment:

```bezel
  public method Write(dest: Stream?, payload: Array<byte>)
    var target: Stream;
    if dest <> null then
      target := dest;          // borrows the caller's stream
    else
      target := _fallback;     // borrows our own field
    end;
    // Merge: `target` is a borrow on both branches, and both sources
    // (the caller's `dest`, or our `_fallback` field) outlive this point.
    target.Write(payload, 0, payload.Length);
    target.Flush();
  end Write;
```

Each branch only *selects* `target` — binding it either to the caller's `dest` or to the long-lived `_fallback` field, never to a branch-local. That is what makes the merge legal: at the post-merge `target.Write(...)`, the borrow's source (`dest` or `_fallback`) is still alive. The actual write happens exactly once, after the merge, through whichever source was chosen.

The mistake to avoid is introducing a short-lived owned stream in a branch and binding `target` to it. Here is the same method written wrongly — it parses and type-checks; only the borrow rule is violated:

```bezel
public method Write(dest: Stream?, payload: Array<byte>)
  var target: Stream;
  if dest <> null then
    target := dest;
  else
    using var probe := new FileStream("tmp", FileMode.Create);
    target := probe;       // probe is disposed at this branch's end...
  end;
  target.Write(payload, 0, payload.Length);   // ...so target dangles here — ERROR
  target.Flush();
end Write;
```

`probe` is discharged when the else-branch closes, so at the post-merge `target.Write(...)` the borrow's source is already gone. The borrow checker rejects this — a borrow may never outlive the source it points into, and a branch merge inherits that rule rather than escaping it. The error is reported at the merge (or at the first post-merge use), naming `target` and the branch whose source does not survive. If a branch genuinely needs a short-lived owned object, let it finish its work and be discharged inside the branch; bind the post-merge variable from something longer-lived (a borrowed parameter, a field, or a value the caller owns).

**Why not just `target := dest ?? new FileStream(...)`?** Earlier in this section, `dest ?? _fallback` *did* work — because both sides were borrows. Replacing `_fallback` with `new FileStream(...)` breaks it, and the contrast is the whole point. `dest` is a borrow (you do not own it); `new FileStream(...)` is owned (you must dispose it). `??` cannot yield "a borrow or an owned value depending on which side ran" — that is the rejected mixed-ownership merge from §7, just written as an expression. The working version coalesced two borrows; this one tries to coalesce a borrow with an owned value, and Bezel makes you resolve the ambiguity the null-coalescing operator would otherwise hide.

`??` is not broken, though — it is exactly right when both sides share ownership. The common case is supplying a default for a runtime-managed value like `string` (which is never owned, so there is no ambiguity):

```bezel
var displayName: string := user?.Name ?? "Anonymous";   // both sides are string
```

`a ?? b` returns `a` when non-null, else `b`; the right side must be compatible with the non-nullable form of the left. It works whenever `a` and `b` agree on ownership — two `string`s, two borrows with a common source, two owned values of the same type. It is rejected only when the two sides would disagree on ownership, which is the same single rule §7 is about. The `?.` (null-conditional) and `??=` (assign-if-null) operators follow the same principle.

**Fixing it inside the method, with no field.** The earlier `ResponseWriter` used an object-scoped field so the fallback outlived the method. You do not need a field — you need the owned fallback to be live *at the point of use*. The cleanest way is to stop trying to make one `target` span the merge, and instead put the write where the fallback is in scope, using the nested-block tool the language provides for exactly this:

```bezel
public method Write(dest: Stream?, payload: Array<byte>)
  if dest <> null then
    dest.Write(payload, 0, payload.Length);   // borrow; caller still owns dest
    dest.Flush();
  else
    using var fallback := new FileStream("tmp", FileMode.Create);
    fallback.Write(payload, 0, payload.Length);
    fallback.Flush();
  end;   // fallback disposed here, on the only path where it exists
end Write;
```

There is no merged `target` variable, so there is no mixed-ownership merge to reject. Each branch uses its own destination while that destination is unambiguously live, and the owned fallback is created and disposed entirely within the branch that needs it — created lazily (never opened when `dest` was supplied) and disposed exactly once.

If the post-merge logic is substantial and you do not want to duplicate it across both branches, factor it into a helper that takes the destination as a *borrowed* parameter, and let each branch call it:

```bezel
public method Write(dest: Stream?, payload: Array<byte>)
  if dest <> null then
    Emit(dest, payload);                       // borrows the caller's stream
  else
    using var fallback := new FileStream("tmp", FileMode.Create);
    Emit(fallback, payload);                    // borrows the local fallback
  end;
end Write;

private method Emit(sink: Stream, payload: Array<byte>)
  sink.Write(payload, 0, payload.Length);
  sink.Flush();
end Emit;
```

`Emit` borrows `sink` (no `owned`), so it imposes no disposal obligation and works identically whether the caller passes a borrowed parameter or a local `using var`. This is the idiomatic shape: the shared logic takes a borrow; each caller supplies a destination whose lifetime it controls. The ownership question is answered once, locally, at each call site — never smuggled through a merge or a `??`.

**Pattern B — make both branches owned.** When the two possibilities are both *owned values of the same type*, you have a choice of form. The `if`-expression (`trueValue if condition else falseValue`) is the idiomatic one and reads best:

```bezel
using var x := new Buffer() if useFresh else source.CloneBuffer();
x.Process();
// Both arms are owned Buffer, so the expression has a single,
// unambiguous ownership: owned. `using var` discharges it at scope end.
```

This works *precisely because both arms have the same ownership*. It is the same principle as the bad merge in §7, seen from the other side: a `value if c else value` expression (and `??`, below) is only well-formed when both operands agree on ownership. Here they do — both are owned — so there is nothing for the compiler to reject.

The statement form is equivalent and is what you fall back to when the branches need more than one expression each. Note it is a *statement* (`if … then … else … end;`), not the ternary above — declare the variable first, assign in each branch:

```bezel
var x: Buffer;
if useFresh then
  x := new Buffer();              // owned
else
  x := source.CloneBuffer();      // returns owned Buffer — also owned
end;
// x is owned on both branches — the merge is fine.
// An owned var still carries the disposal obligation; discharge it by
// binding into a using var, returning it under an owned return type,
// or storing it into an owned field:
using var owned_x := x;           // adopt for scoped disposal
owned_x.Process();
```

Pick A when the value is short-lived and you only need to read it; pick B when the value must outlive the `if`. A `var` that is owned but not yet under `using var` is still tracked — the compiler rejects the program if it is neither disposed nor transferred, so the obligation cannot be silently dropped. The error message at a bad merge names the variable and the disagreeing branches.

---

## 8. Quick reference

| You want to… | Write |
|---|---|
| Create something and clean it up here | `using var x := new T(...);` |
| Use something you don't own | take it as a plain (non-`owned`) parameter |
| Hold a resource inside an object | `private owned _f: T := ...;` |
| Point back at something that outlives you | `private _f: T := ...;` (no `owned`) |
| Hand a new object to the caller | return type `: owned T`, `return x;` |
| Move an owned field's contents out | `move _f` (then reassign `_f` before exit) |
| Merge branches with mixed ownership | equalize first (Pattern A or B, §7) |

Three habits cover almost everything: reach for `using var` by default, mark container-held resources `owned` and back-references plain, and when the compiler rejects a branch join, equalize the branches rather than fighting it — the rejection is almost always finding a real leak.
