# Bezel Guide 1: Memory and Ownership

*A short, example-first guide. Companion to the Bezel Language Specification — re-validated against v0.200.*

Bezel has no garbage collector. Every heap object has exactly one owner responsible for disposing it. The compiler tracks ownership and guarantees every object is disposed exactly once — no leaks, no double-frees. This is an **affine** model: an owned value you do not explicitly discharge is auto-dropped at the end of its scope (with a must-use warning), and a use-after-free or double-free is rejected outright. This guide shows the everyday patterns; it does not restate the full rules (see spec §28).

The whole model reduces to one question for every class-typed value: **am I responsible for disposing this, or is someone else?** Owned means yes. Borrowed means no.

---

## 1. The two declarations you will actually type

```bezel
using var conn := new Connection("db://localhost");  // owned, disposed at block end
var view := conn.View();                             // borrowed, disposes nothing
```

`using var` is the workhorse. It says *I own this, dispose it when this block ends.* Plain `var` takes whatever ownership the right-hand side produced — here `View()` returns a borrow, so `view` is a borrow and carries no obligation.

You never *declare* a local `owned`; a local gets its state from how it is introduced. But you will still type `owned` next to locals often — as the *transfer marker* on a right-hand side or a call argument (`var y := owned x;`, `M(owned x)`) when you hand an owned value on to something that takes it (§28.3). The keyword written *on a declaration* is what's reserved for fields, parameters, and return types (sections 5–7).

The disposal is automatic and ordered. At the end of the enclosing block, `using var` values are disposed in reverse order of declaration, including on the exception path:

```bezel
method Import(path: string)
  using var file := new FileStream(path, FileMode.Open);  // disposed second
  using var parser := new Parser(file);    // disposed first, even if ReadAll throws
  parser.ReadAll();
end Import;
```

You do not write cleanup code. If `ReadAll` throws, both are disposed, innermost first; if the `Parser` constructor itself throws, `file` is still disposed and `parser` — never bound — is not. A `using var` covers everything bound before the failure point.

---

## 2. The four ways to dispose — and the C# habit to unlearn

There is really only one disposal mechanism: at the end of a scope, every owned value still live is disposed — on the normal path and the exception path alike. Everything else is a specialization of that one rule. You will reach for four spellings; they differ only in *when* disposal happens and whether the compiler nudges you:

| You write | Disposes | The compiler… |
|---|---|---|
| `using var x := …` | at scope end, every exit path | says nothing — you declared the intent |
| plain `var x := …`, never moved on | at scope end (auto-drop) | **warns** at the drop — "did you mean to let this go?" |
| `x.Dispose();` | right there, early | says nothing — the scope-end pass skips it |
| `_ := x;` | right there, and ends the binding | says nothing — the discard is the announcement |

`using var` is the default: *lives and dies here.* Plain `var` is for a value whose ownership will **move on** — a factory result you return, an accumulator, the best-so-far in a loop; the must-use warning then marks any path where it silently *doesn't* move on, so a leak is never quiet. An explicit `.Dispose()` is for an **early or conditional** release inside a longer scope. `_ := x;` disposes an unwanted owned value immediately (§9).

**The C# habit to unlearn.** In C#, `foo.Dispose()` at the end of a block is the anti-pattern `using` exists to kill, because a thrown exception skips the trailing call. In Bezel the unwinding edge *always* disposes live owned values, so a trailing `Dispose()` is exception-safe by construction. It therefore never means "cleanup plumbing" — it means one thing only: *release this early.* Auto-drop is the safety net underneath all four forms; you are choosing *when* and *how loudly*, never *whether*.

---

## 3. Borrowing: the default for "just let me use it"

A method that only *uses* an object should borrow it, not own it. Borrowing is the absence of `owned`:

```bezel
method PrintSummary(account: BankAccount)        // borrows account
  Console.WriteLine(account.Balance.ToString());
end PrintSummary;                                // disposes nothing
```

The caller keeps ownership; `PrintSummary` cannot dispose `account` and cannot return it. It *can* stash a reference under the back-reference discipline of §5 — a plain, non-owned field whose target is guaranteed to outlive the holder — but it cannot take ownership. This is the common case — most methods borrow.

A borrow is valid only while its *source* stays alive. The compiler enforces this; you cannot return a borrow that outlives what it points into:

```bezel
method Leak(): View
  using var c := new Connection("...");
  return c.View();        // ERROR: borrow of c, but c is disposed at block end
end Leak;
```

The fix is to return an owned value instead (section 6), or to take the source as a borrowed parameter so the caller owns it.

---

## 4. How an expression's ownership is decided

You rarely think about this explicitly, but it is worth seeing once. The right-hand side of an assignment has an ownership state determined by its form:

```bezel
using var a := new Buffer();          // new T(...)            -> owned
var b := source.OpenBuffer();         // returns owned Buffer   -> owned
var c := source.Buffer;               // field / property read  -> borrowed (source: source)
var d := registry.Find("key");        // returns Buffer (no owned) -> borrowed
var t := source.PrefetchAsync();      // returns Task<Buffer>  -> owned (task calls mint, v0.190)
var e := move holder.slot;            // move out of owned field -> owned
```

The rule of thumb: `new` and any method whose return type says `owned` give you an owned value (you must discharge it). A plain field read or a plain (non-`owned`) return from an *instance* method is a borrow (you must not). Three refinements cover the rest: a *static* method's plain return is treated as owned, with a warning nudging the API author toward writing `owned` (the factory safety net); a call whose declared return type is `Task`/`Task<T>`/`OwningTask<T>` is **owned** no matter the callee — task-returning calls mint a fresh frame at every call, no marker needed (v0.190); and a generic call returning bare `T`, like `Math.Max(a, b)`, is a **borrow of its arguments** (v0.194). When in doubt, the spec's producers table (§28.3) is exhaustive.

---

## 5. Owning fields: objects that hold other objects

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
- **A class whose only resources are `owned` fields needs no destructor.** The compiler-generated cleanup disposes them in reverse declaration order (a nullable field is null-checked first — a null field disposes nothing). Write a destructor only for native/foreign resources.

`owned` is only for class- or interface-typed fields. `string` and function-typed fields are runtime-managed and cannot be `owned` (the compiler will tell you).

---

## 6. Returning ownership: factory and transfer methods

If a method *creates* something the caller must own, say so with an `owned` return type:

```bezel
method CloneBuffer(): owned Buffer
  var fresh := new Buffer();   // owned, because `new` produces an owned value
  fresh.CopyFrom(_source);
  return fresh;                // the owned value reaches the `owned` return — done
end CloneBuffer;
```

Note this is plain `var`, not `using var`. Ownership is tracked on the value no matter how the local is declared — `new` produces an owned value. An owned value *should* reach a discharge point before the method ends — an `owned` field, a `using var`, an `owned` parameter, an **`owned` return**, an explicit `.Dispose()`, or the discard `_ := …` — and if it reaches none, it is auto-dropped at scope end and the compiler warns (§2). `return fresh` under the `owned` return type *is* such a discharge point, so the obligation is discharged by the return itself. (Transferring a *named* owned local into another local or a `using var` is spelled with the marker, `:= owned x` — §8.) Nothing is disposed here; ownership transfers to the caller, who writes:

```bezel
using var s := factory.CloneBuffer();   // caller now owns it, disposes it
```

**When would you use `using var` here instead?** Not for this factory shape at all. `using var` means *scope-pinned*: its value is disposed at this block's end and can never be moved out — `using var fresh := new Buffer(); … return fresh;` is a compile error ("cannot move out of `using var`"; spec §28.3). The factory idiom is plain `var`, and it is already exception-safe as written: if `CopyFrom` throws, `fresh` is a live owned value on the unwinding path and the exception-path discharge (spec §28.3) disposes it during unwinding — you write no cleanup code. And if some normal path drops `fresh` without returning it, it auto-drops at scope end with the must-use warning rather than leaking. The division of labor is intent: `using var` when the value should live and die in this scope; plain `var` when ownership will move on.

This is the one legal way to *transfer* an owned value past a method boundary. A method may also hand out a **borrow** of an owned field: a *bare read* of the field, returned through a **non-`owned`** return type, borrows the field (spec §28.6). The caller may read it but may not dispose it, store it as owned, or use it past the receiver's lifetime — the field's owner still disposes it. To hand the caller *ownership*, `move` the field out through an `owned` return type:

```bezel
method GetTransactions(): Array<Transaction>
  return _transactions;       // OK: borrows the field — the caller may read
                              //   it but not dispose or store it as owned.
end GetTransactions;

method TakeTransactions(): owned Array<Transaction>
  return move _transactions;  // transfer: hand the caller ownership; the field is
                              //   left moved-from and must be restored (next section).
end TakeTransactions;
```

So a caller that needs to *look at* `_transactions` gets a borrow (the bare-field-read return above, or a dedicated borrowing view); a caller that must *take* it uses `move` with an `owned` return (next section).

**Returning a lazy pipeline is fine — over your own inputs.** A method may build and return a lazy query (or an iterator, or a `Task`) over its *own parameters or receiver* — `method Evens(xs: Array<int>): IEnumerable<int> => xs.Where(IsEven);` — even though the returned frame borrows them. The caller re-establishes that borrow against its *own* argument and must keep it alive while it enumerates — for a returned `Task`, until the task settles (and the returned task is itself caller-owned: task calls mint, v0.190, so it is discharged like any other owned value). What you may *not* return this way is a pipeline over a local you created inside the method; that borrow would dangle — materialize it into an `owned Array<T>` instead.

---

## 7. `move`: handing an owned field's contents away

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
// A non-nullable owned field this time: private owned _log: Logfile;
method Rotate()
  var fresh := new Logfile(NextName());  // build the replacement first — it may throw
  using var old := move _log;            // then take — nothing throwable follows
  _log := fresh;                         // restore — a store into a moved-from slot cannot throw
  old.Flush();
end Rotate;                              // old disposed here
```

Forgetting to reassign `_log` is a compile error, not a runtime surprise. So is restoring *via* something that can throw: a call between the move and the reassignment puts an exception edge there, and on that edge the method would exit with the field still moved-from — hence the *build before take* order above (§28.8.1, “Exception paths are not exempt”). The same `move` form works on owning-collection slots (`move buffer[i]`); see spec §28.8.1 (the move expression) and §28.11 (collection element ownership).

---

## 8. The one rule that will actually trip you: branch joins

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
    target.Write(payload, 0, payload.Count);
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
    target.Write(payload, 0, payload.Count);
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
  target.Write(payload, 0, payload.Count);   // ...so target dangles here — ERROR
  target.Flush();
end Write;
```

`probe` is discharged when the else-branch closes, so at the post-merge `target.Write(...)` the borrow's source is already gone. The borrow checker rejects this — and it rejects it *early*, right at `target := probe;`: a borrow may not be assigned to a variable whose scope outlives the borrow's source (here `target` is method-scoped, `probe` lives only in the else-branch), so the diagnostic points at that assignment and names the source `probe` that `target` would outlive — you do not have to wait for the post-merge use to see it. (The genuinely *mixed* owned/borrowed merge at the start of this section is the one still reported at the merge point.) If a branch genuinely needs a short-lived owned object, let it finish its work and be discharged inside the branch; bind the post-merge variable from something longer-lived (a borrowed parameter, a field, or a value the caller owns).

**Why not just `target := dest ?? new FileStream(...)`?** Earlier in this section, `dest ?? _fallback` *did* work — because both sides were borrows. Replacing `_fallback` with `new FileStream(...)` breaks it, and the contrast is the whole point. `dest` is a borrow (you do not own it); `new FileStream(...)` is owned (you must dispose it). `??` cannot yield "a borrow or an owned value depending on which side ran" — that is the rejected mixed-ownership merge from §8, just written as an expression. The working version coalesced two borrows; this one tries to coalesce a borrow with an owned value, and Bezel makes you resolve the ambiguity the null-coalescing operator would otherwise hide.

`??` is not broken, though — it is exactly right when both sides share ownership. The common case is supplying a default for a runtime-managed value like `string` (which is never owned, so there is no ambiguity):

```bezel
var displayName: string := user?.Name ?? "Anonymous";   // both sides are string
```

`a ?? b` returns `a` when non-null, else `b`. The right side may be `T` or `T?` — the result is nullable exactly when the right side is, so `user?.Name ?? "Anonymous"` is a plain `string`, while `primary ?? fallback` over two `string?`s stays `string?`. It works whenever `a` and `b` agree on ownership — two `string`s, two borrows (their sources need not match; the result is a borrow you use while both stay live), or two owned values *freshly produced in the arms*. Writing two *named* owned locals as the arms makes it a borrow merge instead — a named owned binding read as an arm is a borrow, never a silent consume — so to coalesce two owned locals into one owned result you use an explicit `if`/`else` with `owned` transfers. `??` is rejected only when the two sides disagree on ownership, which is the same single rule §8 is about. The `?.` (null-conditional) and `??=` (assign-if-null) operators follow the same principle.

**Fixing it inside the method, with no field.** The earlier `ResponseWriter` used an object-scoped field so the fallback outlived the method. You do not need a field — you need the owned fallback to be live *at the point of use*. The cleanest way is to stop trying to make one `target` span the merge, and instead put the write where the fallback is in scope, using the nested-block tool the language provides for exactly this:

```bezel
public method Write(dest: Stream?, payload: Array<byte>)
  if dest <> null then
    dest.Write(payload, 0, payload.Count);   // borrow; caller still owns dest
    dest.Flush();
  else
    using var fallback := new FileStream("tmp", FileMode.Create);
    fallback.Write(payload, 0, payload.Count);
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
  sink.Write(payload, 0, payload.Count);
  sink.Flush();
end Emit;
```

`Emit` borrows `sink` (no `owned`), so it imposes no disposal obligation and works identically whether the caller passes a borrowed parameter or a local `using var`. This is the idiomatic shape: the shared logic takes a borrow; each caller supplies a destination whose lifetime it controls. The ownership question is answered once, locally, at each call site — never smuggled through a merge or a `??`.

**Pattern B — make both branches owned.** When the two possibilities are both *owned values of the same type*, you have a choice of form. The `if`-expression (`trueValue if condition else falseValue`) is the idiomatic one and reads best:

```bezel
using var x := new Buffer() if useFresh else source.CloneBuffer();
x.Process();
// CloneBuffer is declared `CloneBuffer(): owned Buffer`, so both arms are
// owned Buffer — the expression has a single, unambiguous ownership: owned.
// `using var` discharges it at scope end.
```

This works *precisely because both arms have the same ownership*. It is the same principle as the bad merge in §8, seen from the other side: a `value if c else value` expression (and `??`, above) is only well-formed when both arms agree on ownership. Here they do — both are fresh owned producers — so there is nothing for the compiler to reject. (Write two *named* owned locals as the arms and the expression becomes a borrow merge, exactly as with `??`; coalescing them into one owned result then takes the explicit `if`/`else` below.)

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
using var owned_x := owned x;     // adopt for scoped disposal (transfer marker)
owned_x.Process();
```

Pick A when the value is short-lived and you only need to read it; pick B when the value must outlive the `if`. A `var` that is owned but not yet under `using var` is still tracked — if it is neither disposed nor transferred, it auto-drops at scope end and the compiler raises the must-use warning (spec §28.3), so a drop is never silent. The mixed-ownership *merge* itself remains a hard error; its message names the variable and the disagreeing branches.

---

## 9. More rules you'll meet: draining, yielding, discarding, capturing

A few everyday situations follow directly from the model; each is one habit.

**Draining an owning collection — adopt each element.** `Drain()` hands you *owned* elements, one per turn. Adopt each into a `using var` on the loop body's first line:

```bezel
for var conn in pool.Drain() do
  using var c := owned conn;      // adopt: disposed at end of this iteration
  c.Send(heartbeat);
end;
```

Forget the adoption and the loop variable auto-drops each iteration with a per-iteration must-use warning — the compiler pointing you at that `using var … := owned …` line. (Plain `for … in pool do …` without `Drain()` yields *borrows*, and the pool keeps its elements.)

**Yielding an owned local — adopt, then yield.** Same shape: pin a named owned value before yielding it.

```bezel
using var l := owned line;        // warning-free
yield return l;
```

In a *borrowing* iterator the bare `yield return line;` also compiles (it re-borrows the frame-resident value) but leaves the must-use warning on `line`. In an *owning* iterator that bare yield instead *consumes* `line` — the owning return type is the announcement, so no marker is needed.

**Frame-disposed `owned` parameters — adopt on entry.** An `owned` parameter of an iterator or async method is disposed by the frame at teardown, but the must-use warning still fires at the parameter; `using var r := owned reader;` on the first line declares the intent and silences it.

**Consuming a `choice` outcome — match the fresh call, adopt the payload.** A `case` directly over a call returning an owning `choice` (spec §35) is an *owning match*: the selected arm's `owned` payload binding is yours, and the adopt idiom applies inside the arm — while a `case` over a *named* binding just peeks (borrows), and consuming a named binding is spelled `case owned outcome of …`, the same marker discipline as every other transfer:

```bezel
case Connect(host) of                 // fresh call: owning match
  Ok(var c) => do
    using var conn := owned c;        // adopt; disposed at block end
    conn.Send(payload);
  end,
  Error(var m) => Log(m)
end;
```

**Three one-line rules.** `_ := x;` disposes `x` *now* and ends the binding — an announced consume, no marker; afterward `x` is treated exactly as after an explicit `.Dispose()`, so a later read is flagged the same way (a straight-line read is a must-use warning plus a checked-mode fault; a read after a *conditional* discard — one branch consumed, another still owning — is a compile error at the merge; see Guide 2 §4). A **closure** may *read* a captured owned value but never dispose, reassign, or move it out; a callback that must consume takes an `owned` parameter. And `Dispose()` needs a receiver you *own* — disposing a borrowed parameter, `this`, or a borrowed element is a compile error (the owner disposes it). The lone exception is per-element cleanup on a *borrowing* collection whose elements you semantically own (`for var e in _index do e.Value.Dispose(); end;`, or `DisposeElements()`) — and even that does not extend to a lazy query over the collection (`for var x in xs.Where(p) do x.Dispose(); end;` is rejected — iterate `xs` directly).

---

## 10. Quick reference

| You want to… | Write |
|---|---|
| Create something and clean it up here | `using var x := new T(...);` |
| Use something you don't own | take it as a plain (non-`owned`) parameter |
| Hold a resource inside an object | `private owned _f: T := ...;` |
| Point back at something that outlives you | `private _f: T := ...;` (no `owned`) |
| Hand a new object to the caller | return type `: owned T`, `return x;` |
| Hand an owned local to a method that takes it | `M(owned x)` (marker required) |
| Move an owned field's contents out | `move _f` (then reassign `_f` before exit) |
| Dispose an unwanted owned value now | `_ := x;` (or `x.Dispose();` for an early release) |
| Drain an owning collection | `for var e in coll.Drain() do using var x := owned e; … end;` |
| Merge branches with mixed ownership | equalize first (Pattern A or B, §8) |

Three habits cover almost everything: reach for `using var` by default, mark container-held resources `owned` and back-references plain, and when the compiler rejects a branch join, equalize the branches rather than fighting it — the rejection is almost always finding a real leak. Everything else — the explicit `.Dispose()`, the discard, the `owned` transfer marker — is just choosing *when* a disposal happens; auto-drop guarantees it always does.
