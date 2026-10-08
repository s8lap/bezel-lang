# Bezel Guide 1: Memory and Ownership

*A short, example-first guide. Companion to the Bezel Language Specification — re-validated against v0.327.*

Bezel has no garbage collector. Every heap object has exactly one owner responsible for disposing it. The compiler tracks ownership and guarantees every object is disposed exactly once — no leaks, no double-frees. This is an **affine** model: an owned value you do not explicitly discharge is auto-dropped at the end of its scope, with a must-use warning (§2's trivial-dispose classes excepted). A use after disposal is caught in layers: a borrow that would outlive its local source is rejected, a use of a variable after its explicit disposal is a warning on a straight line and a compile error after a conditional one, and in the default, checked build any member access through a disposed reference faults deterministically (spec §28.2). This guide shows the everyday patterns; it does not restate the full rules (see spec §28).

The whole model reduces to one question for every class-typed value: **am I responsible for disposing this, or is someone else?** Owned means yes. Borrowed means no.

---

## 1. The two declarations you will actually type

```bezel
using var conn := new Connection("db://localhost");  // owned, disposed at block end
var view := conn.View();                             // borrowed, disposes nothing
```

`using var` is the workhorse. It says *I own this, dispose it when this block ends.* Plain `var` takes whatever ownership the right-hand side produced. Here `View()` returns a borrow, so `view` is a borrow and carries no obligation.

You never *declare* a local `owned`; a local gets its state from how it is introduced. The two keywords split cleanly by role: **`owned` describes a slot** — it appears only on declarations (fields, parameters, return types; sections 5–7) and answers "what obligation does this location hold?" — while **`move` performs the action**: it is the *transfer marker* on a right-hand side or a call argument — `var y := move x;`, `M(move x)` — when you hand an owned value on to something that takes it, and the same verb that empties a field or slot (`move x.f`, §7). Wherever you see `move`, a named location is being emptied, now, here (§28.3).

The disposal is automatic and ordered. At the end of the enclosing block, `using var` values are disposed in reverse order of declaration, including on the exception path:

```bezel
method Import(path: string)
  using var file := new FileStream(path, FileMode.Open);  // disposed second
  using var parser := new Parser(file);    // disposed first, even if ReadAll throws
  parser.ReadAll();
end Import;
```

You do not write cleanup code. If `ReadAll` throws, both are disposed, innermost first. If the `Parser` constructor itself throws, `file` is still disposed; `parser`, never bound, is not. A `using var` covers everything bound before the failure point.

---

## 2. The four ways to dispose — and the habit to unlearn

There is really only one disposal mechanism. At the end of a scope, every owned value still live is disposed — on the normal path and the exception path alike. Everything else is a specialization of that one rule. You will reach for four spellings — `using var x := …`, a plain `var x := …` never moved on, `x.Dispose();` and the discard `_ := x;` — and they differ only in *when* disposal happens and whether the compiler nudges you; spec §28.3's *Choosing a disposal form* and the code-generation reference's §4.2 *Disposal-form menu* tabulate them.

`using var` is the default: *lives and dies here.* Plain `var` is for a value whose ownership will **move on** — a factory result you return, an accumulator, the best-so-far in a loop. The must-use warning then marks any path where it silently *doesn't* move on, so a leak is never quiet. An explicit `.Dispose()` is for an **early or conditional** release inside a longer scope. `_ := x;` disposes an unwanted owned value immediately (§9).

**One owned kind takes neither discard form: a `Task`-family value** (`Task`, `Task<T>` or `OwningTask<T>`). `_ := SomeAsync();` is a compile error, and so is the anonymous binding `using var _ := SomeAsync();` — nothing can await an unnamed binding, so it is the same abandonment one scope later. Disposing a task abandons a running operation, which is a decision about the *operation*, not cleanup of a value, so the language makes you announce which decision you mean: `Task.FireAndForget(SomeAsync())` lets it run to completion and logs any failure, and a **named** `using var t := SomeAsync();` is the unsupervised scope-end drop (spec §17.13; stdlib §6.7). Every other owned value takes all four spellings above.

**The habit to unlearn.** Elsewhere, a trailing `foo.Dispose()` at the end of a block is the anti-pattern `using` blocks exist to kill, because a thrown exception skips the trailing call. In Bezel the unwinding edge *always* disposes live owned values, so a trailing `Dispose()` is exception-safe by construction. It therefore never means "cleanup plumbing." It means one thing only: *release this early.* Auto-drop is the safety net underneath all four forms. You are choosing *when* and *how loudly*, never *whether*. And *loudly* has a meaning: disposing a resource is an **effect** — a file closes, a lock releases, a subscription unhooks — and the spellings say where effects fire, the warning firing exactly where none says so.

**When there is nothing to dispose: trivial-dispose classes — a ceremony exemption, never a lifetime one.** A class with no destructor, no `owned` field of non-trivial-dispose type and no owning-inline-array field, on a trivial-dispose base, has a disposal that runs no user code, so the compiler drops the ceremony — no `using var` owed, no must-use warning — but never the tracking: the instance still has one owner, is freed at that owner's release point, and every borrow rule in §3 applies to it (spec §28.9). The everyday sighting is the accumulator, `var sb := new StringBuilder(); … return sb.ToString();`, with no `using var` and no warning. Where the exemption reaches, why the classification is part of a public class's contract, and why a fresh trivial-dispose value handed to a borrowing collection still warns are spec §28.9 and the code-generation reference's §4.1 and §4.5.

---

## 3. Borrowing: the default for "just let me use it"

A method that only *uses* an object should borrow it, not own it. Borrowing is the absence of `owned`:

```bezel
method PrintSummary(account: BankAccount)        // borrows account
  Console.WriteLine(account.Balance.ToString());
end PrintSummary;                                // disposes nothing
```

The caller keeps ownership; `PrintSummary` cannot dispose `account` and cannot return it. It *can* stash a reference under the back-reference discipline of §5 — a plain, non-owned field whose target is guaranteed to outlive the holder — but it cannot take ownership. This is the common case: most methods borrow.

A borrow is valid only while its *source* stays alive. The compiler enforces this; you cannot return a borrow that outlives what it points into:

```bezel
method Leak(): View
  using var c := new Connection("...");
  return c.View();        // ERROR: borrow of c, but c is disposed at block end
end Leak;
```

The fix is to return an owned value instead (section 6), or to let the caller, who owns the source, take the borrow itself. A method whose return type is a plain class type like `View` can hand out a borrow only of a source every caller can re-establish — its receiver (for an extension method, the receiver parameter) — so a view of a borrowed *parameter* cannot be returned that way either (spec §28.3, *Borrow lifetime rules*).

---

## 4. How an expression's ownership is decided

You rarely think about this explicitly, but it is worth seeing once. The right-hand side of an assignment has an ownership state determined by its form:

```bezel
using var a := new Buffer();          // new T(...)            -> owned
var b := source.OpenBuffer();         // returns owned Buffer   -> owned
var c := source.Buffer;               // field / property read  -> borrowed (source: source)
var d := registry.Find("key");        // returns Buffer (no owned) -> borrowed
var t := source.PrefetchAsync();      // returns Task<Buffer>  -> owned (task calls mint)
var q := xs.Where(p);                 // returns a lazy query  -> owned (calls mint)
var parts := line.Split(':');         // returns owned Array<string> -> owned (trivial-dispose: no ceremony, §2)
var e := move holder.slot;            // move out of owned field -> owned
```

The rule of thumb: `new` and any method whose return type says `owned` give you an owned value (you must discharge it). A plain field read or — outside the mint-announcing families — a plain (non-`owned`) return from an *instance* method is a borrow (you must not). Three refinements cover the rest:

- Outside the mint-announcing families, a *static* method's class-typed return must say `owned` — the compiler rejects the unmarked form, so a static signature always tells you whether you own the result.
- A call whose declared return type is **mint-announcing** — `Task`/`Task<T>`/`OwningTask<T>`, or an iterable/enumerator interface or a lazy query — is **owned** no matter the callee: calls mint a fresh frame, no marker needed. A stored task or collection view is re-exposed as a property, whose read is a borrow.
- A generic call returning bare `T` at a class or interface type argument, like `Math.Max(a, b)` over two class-typed values, is a **borrow of its arguments**.

`Split` is the producer people miss: its result is owned — a fresh array, not a borrow of the string — though `Array<T>` is trivial-dispose, so no ceremony is owed (§2). It also keeps every segment, empty ones included: `"a::b".Split(':')` is `["a", "", "b"]`, and a tokenizer filters explicitly — `line.Split(':').Where(lambda(w) => w.Length > 0)` (stdlib §9.6).

When in doubt, the spec's producers table (§28.3) is exhaustive.

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
- **A class whose only resources are `owned` fields needs no destructor.** The compiler-generated cleanup disposes them in reverse declaration order (a nullable field is null-checked first; a null field disposes nothing). Write a destructor only for native/foreign resources.

`owned` is only for fields of class or interface type (or a bare type parameter). `string` and function-typed fields are runtime-managed and cannot be `owned` (the compiler will tell you).

**The back-reference's guarantee is yours — unless you say `scoped`.** `_owner` above is admitted because it comes from a constructor parameter, and that is exactly how much the compiler checks: a borrow of a *local* may never land in a field (the local dies at scope end, which the object outlives), while a borrow of a plain parameter or of `this` may (spec §28.3). Whether the `Customer` really outlives the `BankAccount` is *not* checked — it is a recorded trust clause (spec §28.3; kernel K4.3), and a wrong guess is a `DisposedAccessException` in the default, checked build (spec §28.2) and undefined behavior only in a build that explicitly selects unchecked disposal. That is the right trade for most back-references, and it is what this guide's examples use — and when you want to see every place you are making it, build in the trust-admission report mode (spec §28.3; the compiler's `--report-trust-admissions` switch): each such store draws a warning naming the source you are trusting and the static spellings that would replace the trust. When you want the compiler to *prove* the lifetime instead, mark the parameter `scoped` — and the argument too:

```bezel
class Statement
  private _account: BankAccount;                       // a plain field, exactly as before
  public constructor(scoped account: BankAccount)
    _account := account;                               // OK: a plain field of the object being built
  end constructor;
  public method Print()
    Console.WriteLine(_account.Balance.ToString());
  end Print;
end Statement;

method PrintStatement(customer: Customer)
  using var acct := new BankAccount(customer);         // the source: declared BEFORE the holder
  using var stmt := new Statement(scoped acct);        // the marker at the call site, too (spec §22.3)
  stmt.Print();
end PrintStatement;                                    // stmt disposed first, then acct — the order the proof relies on
```

Three things together make this static: the source must be declared *before* the holder, the source is **frozen** while the holder lives (no `move`, no reassignment, no `acct.Dispose()`), and locals are disposed in reverse declaration order. So `acct` provably outlives `stmt`, its destructor included — spec §28.14 calls this fully static with no residual trust; checked mode is defense-in-depth only. The `scoped` keyword changes nothing about the field: it is the same plain back-reference, and a plain (non-`scoped`) parameter stored the §5 way stays legal under the trust regime. A source that is itself a plain *parameter* of the enclosing method needs no freeze — it already outlives every local.

The shortest spelling of the same holder is a primary constructor: `class Statement(scoped account: BankAccount) … end Statement;` — a body that reads `account` gets the plain back-reference field synthesized for it, and the call site, the `using var` holder, the freeze and the disposal order are exactly as above (spec §22.10).

Three shapes the compiler rejects, each with a one-line remedy. `new Statement(scoped new BankAccount(customer))` — a fresh temporary dies at the end of the statement; bind it to a `using var` first. `var stmt := new Statement(scoped acct)` — the holder must be a `using var` local (or `this`) so the compiler has a lifetime to bound by; a plain `var` holder admits no borrow argument at all. And inside `Statement`, the scoped parameter may be stored in a plain field of `this` or forwarded to another `scoped` parameter on `this`, never returned or stored in another object (spec §28.14).

When to reach for it: holders that are built and discarded next to their source — readers, cursors, views, guards. When the holder will outlive the scope it was built in, the freeze would outlive it too; keep the plain form there and let checked mode stand behind it.

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

Note this is plain `var`, not `using var`. Ownership is tracked on the value no matter how the local is declared — `new` produces an owned value. An owned value *should* reach a discharge point before the method ends: an `owned` field, a `using var`, an `owned` parameter, an **`owned` return**, an explicit `.Dispose()`, or the discard `_ := …`. If it reaches none, it is auto-dropped at scope end and the compiler warns (§2). `return fresh` under the `owned` return type *is* such a discharge point, so the obligation is discharged by the return itself. (Transferring a *named* owned local into another local or a `using var` is spelled with the marker, `:= move x` — §8.) Nothing is disposed here; ownership transfers to the caller, who writes:

```bezel
using var s := factory.CloneBuffer();   // caller now owns it, disposes it
```

**When would you use `using var` here instead?** Not for this factory shape at all. `using var` means *scope-pinned*: its value is disposed at this block's end and can never be moved out. `using var fresh := new Buffer(); … return fresh;` is a compile error ("cannot move out of `using var`"; spec §28.3). The factory idiom is plain `var`, and it is already exception-safe as written. If `CopyFrom` throws, `fresh` is a live owned value on the unwinding path, and the exception-path discharge (spec §28.3) disposes it during unwinding — you write no cleanup code. And if some normal path drops `fresh` without returning it, it auto-drops at scope end with the must-use warning rather than leaking. The division of labor is intent: `using var` when the value should live and die in this scope; plain `var` when ownership will move on.

This is the one legal way to *transfer* an owned value past a method boundary. A method may also hand out a **borrow** of an owned field: a *bare read* of the field, returned through a **non-`owned`** return type, borrows the field (spec §28.6). The caller may read it but may not dispose it, store it as owned, or use it past the receiver's lifetime. The field's owner still disposes it. To hand the caller *ownership*, `move` the field out through an `owned` return type:

```bezel
method GetTransactions(): Array<Transaction>
  return _transactions;       // OK: borrows the field — the caller may read
                              //   it but not dispose or store it as owned.
end GetTransactions;

method TakeTransactions(): owned Array<Transaction>
  var fresh := new Array<Transaction>();  // build the replacement first (next section)
  var taken := move _transactions;        // take: the field is now moved-from
  _transactions := fresh;                 // restore — every exit path must (spec §28.8.1)
  return taken;                           // transfer: hand the caller ownership
end TakeTransactions;
```

So a caller that needs to *look at* `_transactions` gets a borrow (the bare-field-read return above, or a dedicated borrowing view); a caller that must *take* it uses `move` with an `owned` return (next section).

**Returning a lazy pipeline is fine — over your own inputs.** A method may build and return a lazy query (or an iterator, or a `Task`) over its *own parameters or receiver*, even though the returned frame borrows them: `static method Evens(xs: Array<int>): IEnumerable<int> => xs.Where(IsEven);` (an instance or extension form is the same contract in the same unmarked spelling — every iterable-family call mints, whatever the callee kind; spec §28.3 — with `owned` admissible as emphasis). The caller re-establishes that borrow against its *own* argument and must keep it alive while it enumerates — for a returned `Task`, until the task settles. The returned task is itself caller-owned (task calls mint, spec §28.3), so it is discharged like any other owned value. What you may *not* return this way is a pipeline over a local you created inside the method; that borrow would dangle. Materialize it instead — but only where materializing actually gives the elements an owner: at a value element type an `owned Array<T>` is the answer, while at a class or interface element type `ToArray()` returns a *borrowing* array whose slots still borrow the local's elements and dangles exactly as the pipeline did (stdlib §5). There, hand the ownership out: build the elements into an owning collection and return it, or expose them through an owning enumeration (`IOwningEnumerable<T>` — `Drain()` where the source is already an owning collection).

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

`move _log` reads `_log` as an owned value and leaves the field moved-from. The **field-restoration rule** applies here too: every path to the end of the method must put something back:

```bezel
// A non-nullable owned field this time: private owned _log: Logfile;
method Rotate()
  var fresh := new Logfile(NextName());  // build the replacement first — it may throw
  using var old := move _log;            // then take — nothing throwable follows
  _log := fresh;                         // restore — a store into a moved-from slot cannot throw
  old.Flush();
end Rotate;                              // old disposed here
```

Forgetting to reassign `_log` is a compile error, not a runtime surprise. So is restoring *via* something that can throw. A call between the move and the reassignment puts an exception edge there, and on that edge the method would exit with the field still moved-from — hence the *build before take* order above (§28.8.1, “Exception paths are not exempt”). The same `move` form works on owning-collection slots (`move buffer[i]`) — and, as the transfer marker, on named bindings (`move x`; §1, §8). See spec §28.8.1 (the move expression) and §28.11 (collection element ownership).

---

## 8. The one rule that will actually trip you: branch joins

This is the most common compile error newcomers hit. If two branches of an `if` leave the same variable in *different* ownership states, the merge is rejected: one path has a disposal obligation the other does not, and silently dropping it would leak:

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

`dest ?? _fallback` reads: use `dest` if the caller supplied one, otherwise our `_fallback` field. Both are borrows, so the result is a borrow. `target`'s source is whichever of the two long-lived objects was selected; both outlive the method, so the post-merge use is sound. This is the same code as the explicit form below, written the way you would reach for in most languages. Here it happens to be legal because the ownership matches.

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

`probe` is discharged when the else-branch closes, so at the post-merge `target.Write(...)` the borrow's source is already gone. The borrow checker rejects this — and it rejects it *early*, right at `target := probe;`. A borrow may not be assigned to a variable whose scope outlives the borrow's source; here `target` is method-scoped and `probe` lives only in the else-branch. So the diagnostic points at that assignment and names the source `probe` that `target` would outlive — you do not have to wait for the post-merge use to see it. (The genuinely *mixed* owned/borrowed merge at the start of this section is the one reported at the merge point.) If a branch genuinely needs a short-lived owned object, let it finish its work and be discharged inside the branch. Bind the post-merge variable from something longer-lived: a borrowed parameter, a field, or a value the caller owns.

**Why not just `target := dest ?? new FileStream(...)`?** Earlier in this section, `dest ?? _fallback` *did* work — because both sides were borrows. Replacing `_fallback` with `new FileStream(...)` breaks it, and the contrast is the whole point. `dest` is a borrow (you do not own it); `new FileStream(...)` is owned (you must dispose it). `??` cannot yield "a borrow or an owned value depending on which side ran" — that is the rejected mixed-ownership merge from §8, just written as an expression. The working version coalesced two borrows. This one tries to coalesce a borrow with an owned value, and Bezel makes you resolve the ambiguity the null-coalescing operator would otherwise hide.

`??` is not broken, though — it is exactly right when both sides share ownership. The common case is supplying a default for a runtime-managed value like `string` (which is never owned, so there is no ambiguity):

```bezel
var displayName: string := user?.Name ?? "Anonymous";   // both sides are string
```

`a ?? b` returns `a` when non-null, else `b`. The right side may be `T` or `T?`, and the result is nullable exactly when the right side is: `user?.Name ?? "Anonymous"` is a plain `string`, while `primary ?? fallback` over two `string?`s stays `string?`. It works whenever `a` and `b` agree on ownership: two `string`s, two borrows (their sources need not match; the result is a borrow you use while both stay live), or two owned values *freshly produced in the arms*. Writing two *named* owned locals as the arms makes it a borrow merge instead — a named owned binding read as an arm is a borrow, never a silent consume; so to coalesce two owned locals into one owned result, use the marked arms — `(if a <> null then move a else move b)` — or an explicit `if`/`else` with `move` transfers (Pattern B below). `??` is rejected only when the two sides disagree on ownership (and the marker is never written in a `??` operand — spec §28.3) — the same single rule §8 is about. The `?.` (null-conditional) and `??=` (assign-if-null) operators follow the same principle.

**Fixing it inside the method, with no field.** The earlier `ResponseWriter` used an object-scoped field so the fallback outlived the method. You do not need a field — you need the owned fallback to be live *at the point of use*. The cleanest way is to stop trying to make one `target` span the merge. Instead, put the write where the fallback is in scope, using the nested-block tool the language provides for exactly this:

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

There is no merged `target` variable, so there is no mixed-ownership merge to reject. Each branch uses its own destination while that destination is unambiguously live. The owned fallback is created and disposed entirely within the branch that needs it: created lazily (never opened when `dest` was supplied) and disposed exactly once.

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

**Pattern B — make both branches owned.** When the two possibilities are both *owned values of the same type*, you have a choice of form. The `if`-expression (`(if condition then trueValue else falseValue)`) is the idiomatic one and reads best:

```bezel
using var x := (if useFresh then new Buffer() else source.CloneBuffer());
x.Process();
// CloneBuffer is declared `CloneBuffer(): owned Buffer`, so both arms are
// owned Buffer — the expression has a single, unambiguous ownership: owned.
// `using var` discharges it at scope end.
```

This works *precisely because both arms have the same ownership*. It is the same principle as the bad merge in §8, seen from the other side: a `(if c then value else value)` expression (and `??`, above) is only well-formed when both arms agree on ownership. Here they do — both are fresh owned producers — so there is nothing for the compiler to reject. (Write two *named* owned locals as **unmarked** arms and the expression becomes a borrow merge, exactly as with `??` — and at a `using var` a compile error, since the declaration would say neither that it adopts the selected local nor that it views it (spec §28.10). To coalesce them into one owned result, mark the arms: `using var best := (if a.Score() > b.Score() then move a else move b);` — each is consumed only on its selected path — or use the explicit `if`/`else` statement below.)

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
using var adopted := move x;      // adopt for scoped disposal (transfer marker)
adopted.Process();
```

Pick A when the value is short-lived and you only need to read it; pick B when the value must outlive the `if`. A `var` that is owned but not yet under `using var` is still tracked. If it is neither disposed nor transferred, it auto-drops at scope end and the compiler raises the must-use warning (spec §28.3) — a drop is never silent. The mixed-ownership *merge* itself remains a hard error; its message names the variable and the disagreeing branches.

---

## 9. More rules you'll meet: draining, yielding, discarding, capturing

A few everyday situations follow directly from the model; each is one habit.

**Draining an owning collection — adopt each element.** `Drain()` hands you *owned* elements, one per turn. Adopt each into a `using var` on the loop body's first line:

```bezel
for var conn in pool.Drain() do
  using var c := move conn;       // adopt: disposed at end of this iteration
  c.Send(heartbeat);
end;
```

Forget the adoption and the loop variable auto-drops each iteration with a per-iteration must-use warning — the compiler pointing you at that `using var … := move …` line. (Plain `for … in pool do …` without `Drain()` yields *borrows*, and the pool keeps its elements.)

**Yielding an owned local — adopt, then yield.** Same shape: pin a named owned value before yielding it.

```bezel
using var l := move line;         // warning-free *at the producer*
yield return l;
```

In a *borrowing* iterator the bare `yield return line;` also compiles (it re-borrows the frame-resident value) but leaves the must-use warning on `line`. Both spellings hand the *enumerator's frame* the element, so a consumer's class-element terminal accessor (`First()`, `Last()`, …) over such an iterator draws its own warning — Guide 5 §6 (spec §29.11). In an *owning* iterator that bare yield instead *consumes* `line` — the owning return type is the announcement (a marker-free position, spec §28.3), so no marker is needed.

**Frame-disposed `owned` parameters — adopt on entry.** An `owned` parameter of an iterator or async method is disposed by the frame at teardown, but at a non-trivial-dispose type the must-use warning still fires at the parameter; `using var r := move reader;` on the first line declares the intent and silences it.

**Consuming a `choice` outcome — match the fresh call, adopt the payload.** A `case` directly over a call returning an owning `choice` (spec §35) is an *owning match*: the selected arm's `owned` payload binding is yours, and the adopt idiom applies inside the arm. By contrast, a `case` over a *named* binding just peeks (borrows). Consuming a named binding is spelled `case move outcome of …` — the same marker discipline as every other transfer:

```bezel
case Connect(host) of                 // fresh call: owning match
  Ok(var c) => do
    using var conn := move c;         // adopt; disposed at block end
    conn.Send(payload);
  end,
  Error(var m) => Log(m)
end;
```

**Letting a callee keep your lambda — `scoped`.** A plain function-typed parameter is *call-bounded*: the callee may invoke it or pass it on, but may not store it — not in a field, not in a collection that outlives the call, not inside a returned closure. Try, and the diagnostic reads: parameter `cb` is call-bounded; declare it `scoped` to store it. The reason is that a closure's real lifetime is the lifetime of whatever it captured, and a plain signature says nothing about that. `scoped` is the signature that does say it: *I will keep this, for as long as the object that stores it lives* (spec §28.14):

```bezel
class Timer
  private scoped _onTick: (() -> ())?;               // a scoped field: invoke, overwrite, clear — never read out
  public method OnTick(scoped handler: () -> ())
    _onTick := handler;                               // OK: a scoped field of the object being subscribed to
  end OnTick;
  public method Tick()
    _onTick?();                                       // invoke if set (spec §7.6): a mutable field never narrows
  end Tick;
end Timer;

using var conn := new Connection("...");             // the captured source: declared BEFORE the timer
using var timer := new Timer();
timer.OnTick(lambda() => conn.Send(heartbeat));      // bounding object: timer, the receiver
timer.Tick();
// conn is frozen while timer lives: no move, no reassignment, no conn.Dispose()
```

The admission is the same one §5 states for a stored borrow, with the lambda's captures in place of the argument: the object that stores the callback must be a `using var` local (or `this`), every captured class-typed source must be declared *before* it, and those sources are frozen until it is disposed. Swap the two `using var` lines above and the `OnTick` call is rejected naming `conn`; bind the timer with a plain `var` and only capture-free lambdas are admitted. Pass a lambda literal or a method reference — those are the forms whose captures the compiler can read at the call site; a function-typed local, a plain function-typed parameter, a field read or a call result is not (spec §28.14). On the callee side the rules are the mirror image: store only in a `scoped` field of the same object, or forward to a `scoped` parameter on `this`; never return it, bind it to a local, or store it in another object. One obligation stays yours: if your class hands the callback to anything that could fire it after the class is disposed — a worker, a registry — sever that at disposal (the stdlib's `EventSource` does so in its destructor).

You have already met the shape in the standard library: `IEvent<T>.Subscribe(scoped handler)` returns an owned `Subscription`, so `using var sub := button.Clicked.Subscribe(lambda(e) => …)` bounds the handler by `sub` (stdlib §6.5); `TaskScope.Spawn(scoped body)` bounds child captures by the scope (Guide 4 §4); and `ScopeGuard`'s constructor parameter is `scoped`, which is why its captures freeze (§10).

**Three one-line rules.** First: `_ := x;` disposes `x` *now* and ends the binding — a marker-free consume, no marker. Afterward `x` is treated exactly as after an explicit `.Dispose()`, so a later read is flagged the same way: a straight-line read is a use-after-dispose warning (spec §28.2) plus a checked-mode fault, while a read after a *conditional* discard — one branch consumed, another still owning — is a compile error at the merge (see Guide 2 §6). Second: a **closure** may *read* a captured owned value but never dispose, reassign, or move it out; a callback that must consume takes an `owned` parameter, and a callee that must *keep* a callback declares the parameter `scoped` (above). Third: `Dispose()` needs a receiver you *own* — disposing a borrowed parameter, `this`, or a borrowed element is a compile error, with no exception (the owner disposes it). If a collection's elements are yours to dispose, say so in the type: an owning-family collection cascades on `Dispose()`, and selective cleanup takes elements out first (`Drain()`, `Pop*`, `move coll[i]`) into bindings you own.

---

## 10. Cleanup that must not run on the success path: `ScopeGuard`

Scope-end cleanup is automatic and needs no ceremony — that is what owned locals and `using var` are (§2). What has *no* automatic form is undoing a **side effect** only if the rest of the method fails, because a side effect is not a value and has no owner: a registration made, shared state mutated, a handle handed to a foreign library, a transaction begun. On success you keep it; on failure you undo it. `finally` runs on both paths, so the undo needs a flag — and this is correct, composes, and is what everyone writes:

```bezel
var committed := false;
try
  Register(id);
  DoWork();
  committed := true;
finally
  if not committed then Unregister(id); end;
end;
```

It is also four extra lines and one mutable boolean at every site, and the `:= true` has to sit at exactly the right statement — a class of bug that is invisible on the success path. **`ScopeGuard` (stdlib §2.21) is that flag, packaged** — the same method, with the commit point a call the reader can see (the compiler does not implement `ScopeGuard` yet, [#4669](https://github.com/s8lap/bezel-lang/issues/4669); until it does, the flag form above is the spelling that compiles):

```bezel
Register(id);
using var g := new ScopeGuard(lambda() => Unregister(id));
DoWork();
g.Dismiss();             // success: keep the registration
```

The undo is the same `Unregister(id)` call the flag version buried in its `finally` — a unit-returning method, so the lambda is `() -> ()` by construction. On any throwing edge the guard's disposal runs the undo (`using var` covers every exit, §2); `Dismiss()` disarms it, and dismissing twice is a no-op. The `scoped` constructor parameter is what lets the guard *store* your lambda (§9; spec §28.14), and the admission has a price you should meet before it bites: **the locals your lambda captures are frozen while the guard is live** — they may not be moved from, reassigned, or disposed. So the build-guard-commit-*hand-it-out* shape does not compile as written:

```bezel
public method Open(): owned Session
  var conn := Connect();
  using var g := new ScopeGuard(lambda() => conn.Rollback());
  Configure(conn);
  g.Dismiss();
  return conn;             // ERROR: conn is frozen while g is live (spec §28.14)
end Open;
```

The remedy is `do … end` (§21.6 — bounding a local's lifetime tighter than the method is exactly what it is for): end the guard's scope first, then transfer:

```bezel
public method Open(): owned Session
  var conn := Connect();
  do
    using var g := new ScopeGuard(lambda() => conn.Rollback());
    Configure(conn);
    g.Dismiss();
  end;                     // g's scope ends; the freeze on conn lifts
  return conn;
end Open;
```

One sentence on why the freeze is the price: a stored callback that could observe a moved-from or disposed source is exactly what the rule exists to prevent — the freeze is what makes the capture sound. (Two further remedies, both idiomatic: express the action as a method reference on `this` — `new ScopeGuard(this.RollbackPending)` — which captures the receiver and leaves locals unfrozen; or keep the resource in a field, where the freeze does not reach — trading the static bound for spec §28.3's field-held residual-trust regime, with checked disposal (spec §28.2) as the backstop.)

When *not* to reach for it: if the thing to undo is a **value**, it has an owner and the owner's disposal is the answer — `ScopeGuard` is for **effects**: registrations, foreign handles, transactions, counters. And if you arrive from a language with `defer`/`errdefer` and wonder where the keyword went: the value half is redundant under this guide's whole model, and the failure half would need an "are we unwinding?" primitive Bezel deliberately lacks — the decline, with its grounds and reopening bar, is rationale §17.11.

---

## 11. Quick reference

| You want to… | Write |
|---|---|
| Create something and clean it up here | `using var x := new T(...);` |
| Use something you don't own | take it as a plain (non-`owned`) parameter |
| Hold a resource inside an object | `private owned _f: T := ...;` |
| Point back at something that outlives you | `private _f: T := ...;` (no `owned`) — the outliving is your promise, checked mode the backstop (§5) |
| Point back at something that outlives you, compiler-proved | `constructor(scoped p: T) _f := p; end constructor;` and `new X(scoped p)` — `using var` holder, `p` declared before it and frozen while it lives (§5) |
| Let a callee keep your lambda | declare the parameter `scoped`, store it in a `scoped` field; call it with a lambda literal (§9) |
| Hand a new object to the caller | return type `: owned T`, `return x;` |
| Hand an owned local to a method that takes it | `M(move x)` (marker required) |
| Take over an owned local or parameter for scope-end disposal | `using var y := move x;` — the unmarked `using var y := x;` over an owned `x` is an error; to look at `x` without taking it, `var y := x;` or `const y := x;` |
| Move an owned field's contents out | `move _f` (then reassign `_f` before exit) |
| Dispose an unwanted owned value now | `_ := x;` (or `x.Dispose();` for an early release) — never a `Task`-family value (§2) |
| Make a resource-free helper (no destructor, no `owned`/inline-owned fields) | just `new` it — trivial-dispose, no `using var` needed (§2) |
| Drain an owning collection | `for var e in coll.Drain() do using var x := move e; … end;` |
| Merge branches with mixed ownership | equalize first (Pattern A or B, §8) |

Three habits cover almost everything. Reach for `using var` by default. Mark container-held resources `owned` and back-references plain. And when the compiler rejects a branch join, equalize the branches rather than fighting it — the rejection is almost always finding a real leak. Everything else — the explicit `.Dispose()`, the discard, the `move` transfer marker — is just choosing *when* a disposal happens; auto-drop guarantees it always does.
