# Proposal 0109 — `BitReader` / `BitWriter`: bit-granular views over byte spans

| | |
|---|---|
| **Status** | under-review — draft (2026-08-20) |
| **Tracking issue** | — |
| **Target module(s)** | `System.Collections.Generic` (stdlib §4) — the [§4.14](../Bezel_Standard_Library.md#sec-owned-buffers-and-spans) byte-buffer-accessor family gains a bit-granular tier, mirrored onto [§4.15](../Bezel_Standard_Library.md#sec-arrayspant-readonlyarrayspant) as the byte tier already is. No language change. |
| **Written against** | spec/stdlib/rationale **v0.263** (v0.262 at HEAD `8a03dd2`, plus the v0.263 landings in the working tree). Every *existing* name cited was re-verified against that tree; every *new* name is marked **▲**. |
| **Currency** | v0.262 (`8a03dd2`) plus the working tree's **v0.263** landings, two of which this draft depends on: **rationale §11.9** (the pattern-position decline, which names 0109) and **stdlib §9.8** (`Range<T>.Contains` gaining `pure`, 0106 slate item iii — which retires half of D5's original argument). **Quoted:** spec §3.6, §9.2, §9.4, §9.7, §30.14, §32.13; stdlib §1, §4.14, §4.15, §4.16, §7.1; rationale §5.15, §11.9, §18. **Cited without quotation:** spec §5.2, §8.6, §9.5, §17.11, §22.3, §28.11, §29.2, §30.1; stdlib §2.12, §3.4, §7.3, §9.8. Re-verification before any ruling must cover spec §3.6 and §9.4 — this proposal's §4 costs audit and D2 both turn on them — plus stdlib §4.14/§4.15/§4.16 and rationale §11.9. |
| **Origin** | Slate item **v** of [proposal 0106](0106-language-lessons-survey.md) §9 (*"`BitReader`/`BitWriter` stdlib proposal"* — confidence **High** on scope, medium on priority), owner-directed 2026-08-20. The lesson is Erlang's bit syntax; 0106 §3.5 carries the verdict and the residual. |

*Unprefixed §-references are **language-spec** references at v0.263; **stdlib** and **rationale** references are marked as such. References to this document's own sections are always written "this proposal's §N" — its self-numbers are all real spec sections, so a bare §N is never a self-reference.*

---

## 1. Motivation

Framed-protocol work is the whole consumer list. 0106 §3.5: *"the live queue is thick with consumers (0001/0007, 0002, 0020, 0029, 0069), each of which will hand-roll the same mask-and-shift ladder — the bug class the endianness family exists to eliminate one granularity up."* A DNS header packs `id:16, qr:1, opcode:4, aa:1, tc:1, rd:1, ra:1, z:3, rcode:4` into four bytes; a TCP header's data-offset and flags share one 16-bit word; DEFLATE ([0069](0069-compression.md)) reads Huffman codes bit-by-bit. None is expressible today except by hand.

**The absence chain, from 0106 §3.5** (*"the nearest feature is byte-granular only … `BitArray` is a bit set, not this"*), re-verified at HEAD:

1. **Nothing narrower than 16 bits.** stdlib §4.14's family starts at `ReadInt16LittleEndian` / `ReadUInt16BigEndian`; there is no 8-bit member and no sub-byte member. A byte is reachable only through the span indexer.
2. **`BitConverter` is whole-width.** stdlib §2.12 offers `FloatToInt32Bits`, `Int32BitsToFloat`, `DoubleToInt64Bits`, `Int64BitsToDouble` and `Reinterpret<TTo>` — all *same-width* reinterpretations of a whole primitive. None extracts a run of bits.
3. **No offset parameters, and the offsets that exist are byte-granular.** stdlib §4.14: *"**No offset parameter.** Every method operates from the start of the span. To read at an offset, slice first: `header.Slice(8).ReadInt32LittleEndian()`"*. `Slice` counts **bytes** — so the doctrine that replaces offsets reaches one position in eight, and nothing spells "start three bits in".
4. **Nothing declines it.** §32.13 item 6 (*"**Unions and bitfields — declined**"*, *"bitfield layout is ABI-quirky"*) declines C bitfields for an **ABI-layout** reason, which says nothing about a reader that has no ABI at all — 0106 §1.3's *"one narrow residual"*.

So today's answer is a hand-rolled ladder at every consumer — `(b[2] >> 3) & 0x0F` per field, byte index, shift and mask each independently wrong-able, plus a second, differently-wrong ladder for writes. That is the bug class rationale §5.15 built the endianness-named accessors to eliminate one granularity up: *"both alternatives invite the class of bugs where someone forgets that a particular format uses a specific byte order."*

**The objection that makes this expensive in Erlang does not transfer.** Erlang needs refcounted sub-binaries because a matched tail must outlive the match; Bezel's span already *is* a lifetime-checked sub-binary — stdlib §4.15, verbatim: *"The `Source` field carries a borrow of the array; per the borrow-bearing-record rule, the span's lifetime is bounded by the source's lifetime."* The machinery Erlang pays for is replaced here by a derived type property the compiler already computes (§3.6 / §9.7).

---

## 2. The design ▲

### D1 — A value record view: not a class cursor, not an extension family

▲`BitReader` and ▲`BitWriter` are **records** carrying a span plus a bit-granular `(BitStart, BitLength)` window — the §4.14/§4.15 span shape with the position counted in bits.

*Not an extension family on the span (the §4.14 shape).* It cannot express the position: `Slice` reaches one position in eight. A view that counts bits is the smallest thing restoring the "slice first, no offset parameter" doctrine.

*Not a class cursor (0027's shape).* [Proposal 0027](0027-splitspans.md) D1 rejected a span-emitting cursor class on its own ground — that returning a span from `Current` would *"propagate the borrow, making the cursor itself borrow-bearing in effect"*, the coupling §29.2 exists to prevent. The rule that decides the same question here is §9.7's: *"A borrow-bearing record value may not be stored in an **instance field of a class**"* — and a `BitReader` class holding a `ReadOnlySpan<byte>` field is that store. 0027's escape — emit `Range<int>`, let the caller re-derive the span — is unavailable, because a bit range is not a `Range<int>` over the **byte** span the caller holds (D7's `Slice(r: Range<int>)` takes bit indices *into an existing view*, which is a different thing from a range the caller could index the source with).

*Not the `IReadOnlySpan<byte>` interface as the field type.* stdlib §4.16 forecloses it — *"A method declaring `IReadOnlySpan<TElement>` cannot store the parameter in a field that outlives the call"* — and §8.6 bars storing a boxed borrow-bearing record in a field. The interface is a **parameter** type. (It is also why the write side cannot be unified across span families: stdlib §4.16 records that no writable-span interface exists, deliberately.)

### D2 — Advance: reads are position-free, the advance is the view's own bit-granular `Slice`

Every read starts at bit 0 of the view; repositioning is a separate `Slice` returning a new view. The sequential idiom is slice-and-walk:

```bezel
var r := header.AsBitReader();
var id     := ushort(r.ReadBitsBigEndianMsbFirst(16));  r := r.Slice(16);
var qr     := r.ReadBitMsbFirst();                      r := r.Slice(1);
var opcode := byte(r.ReadBitsBigEndianMsbFirst(4));     r := r.Slice(4);
```

*Not a self-advancing reader — but the bar is narrower than it looks, and the rest is judgment.* §9.4 bars a cursor **field of the receiver**: in every instance member body of a value kind *"`this` binds a **borrow of the receiver value**"*, so *"a store that would land in the **receiver's own storage** is a **compile error at the member body**"*. A read bumping its own `BitStart` is exactly that store, and 0106 §3.5 states the correction in advance: *"a record member cannot write a cursor into its own receiver — §9.4 — so the advance idiom is the span family's slice-and-walk shape, not a self-advancing reader."*

What §9.4 does **not** bar is a *mediated* cursor — the position held in a heap cell the record borrows, so the store lands beyond the class hop, admitted by the very sentence this proposal invokes below to make ▲`BitWriter` writable. That shape is legal Bezel, and it is declined on **design** grounds, not language ones: it allocates per view (the whole family is otherwise allocation-free record construction); the cell is shared by every copy of a value type, so `var r2 := r;` produces two handles that advance each other — the aliasing surprise records exist to avoid; and it makes a record whose value is not its state, defeating the auto-equality and `with` semantics §9.4's own "inadvisable mutable records" caution is about. Recorded as a decision, and carried into Q3 as option (c) so the owner rules on it rather than inheriting it.

*Not a fused form returning the advanced view.* Two fused shapes exist. The `out`-carried one — `r := r.ReadBitsBigEndianMsbFirst(16, out id)` — names each width once, but inverts the stdlib's `out` convention (stdlib §1 makes `out` the **failure-shaped** payload channel; no member of the library delivers a *success* payload through `out` while returning something else) and makes every read an assignment to the cursor, so a discarded read still rewrites `r` and a peek needs a second family. The **named-record** one — ▲`record BitRead(Value: ulong, Rest: BitReader)` returned from each read — is the shape rationale §18's product doctrine points at (tuples being declined, the positional record *is* Bezel's product), stores nothing in the receiver, and is a legal borrow-bearing return under §9.7. It loses on cost rather than legality: a fifth and sixth ▲ type (the writer needs its own), two field reads and a destructuring binding at every call site, and a `Try*` twin problem — the Try shapes take a payload, and a `BitRead` payload would have to carry an invalid `Rest` on the false path. Both are carried into Q3 as options (b) and (d).

*The residual cost, plainly.* Each width appears twice, and a mismatch between the read width and the `Slice` width is silent corruption, not a fault. Mitigations: both numbers sit on one line, and an over-run faults (`IndexOutOfRangeException`) or returns `false` from the `Try*` twin rather than reading adjacent data. This is the proposal's sharpest open question — Q3.

*What the store rule permits, for the writer.* §9.4's mediating shape is what makes ▲`BitWriter` writable at all: a chain hopping *"through a class- or interface-typed member leaves the receiver"*, so the store *"lands in the borrowed heap object, not in the receiver, and is **legal**"* — the shape of `Span<T>`'s own indexer setter storing into the `OwningBuffer<T>` its `Buffer` field borrows. ▲`BitWriter`'s store is one hop longer (`this.Source[i] := v` through the record-typed `Source`, whose setter performs the class hop); it still lands in the borrowed buffer and never in the writer's storage, so the rule's key ("where the store would land") admits it, but the spec's example does not literally show this chain. **Recorded as a choice, per CLAUDE.md rule 4:** should the front end read §9.4 as keying on the *immediate* field rather than the landing site, the fallback flattens the writer to `(Buffer: OwningBuffer<byte>, BitStart: int, BitLength: int)` — the spec's own example shape, at the cost of `Slice` reuse. **No *method* signature changes** under the fallback; the positional component does change (`Source: Span<byte>` → `Buffer: OwningBuffer<byte>`, and with it one auto-generated property), `AlignedTail()` constructs rather than slices, and the §4.15 mirror needs its own third shape — so it is a real, if contained, cost.

### D3 — Names carry **both** axes, and the two named families are the two that reduce to §4.14

Bit order *and* byte order are in the method name — `…BigEndianMsbFirst`, `…LittleEndianLsbFirst` — never a parameter, never a constructor argument, never a default.

*Why names.* rationale §5.15: the methods *"name the endianness … rather than taking an endianness parameter or relying on a platform default; both alternatives invite the class of bugs where someone forgets that a particular format uses a specific byte order"* — stdlib §4.14's **"Endianness in the method name … There is no platform-default version and no enum parameter —"** *both alternatives being the bug source the rationale names.* A constructor parameter is strictly worse than the enum parameter that rule already rejects: it moves the decision *further* from the call site that depends on it.

*Why two families, not four.* The axes are independent, so four combinations are nameable. Only two are proposed, because those two obey an **agreement law** with the accessors that exist:

> At a byte-aligned position, for `count` in {16, 32, 64}, ▲`ReadBitsBigEndianMsbFirst(count)` yields exactly what stdlib §4.14's `ReadUInt16BigEndian` / `ReadUInt32BigEndian` / `ReadUInt64BigEndian` yields there, and ▲`ReadBitsLittleEndianLsbFirst(count)` exactly what the matching `…LittleEndian` accessor yields.

Worked, so no reviewer re-derives it: at aligned byte index *B* with `count` = 16, MsbFirst accumulates `b[B]` then `b[B+1]`, giving `b[B] << 8 | b[B+1]` — `ReadUInt16BigEndian`; LsbFirst gives `b[B] | b[B+1] << 8` — `ReadUInt16LittleEndian`. The same walk holds at 32 and 64.

That is the justification for the compound names: each is the bit-granular *generalization* of a §4.14 accessor, and the name says which. The mixed combinations (`BigEndianLsbFirst`, `LittleEndianMsbFirst`) are **declined for now, not foreclosed** — on the agreement law alone: they generalize no accessor that exists, so their names would promise a correspondence there is nothing to check against, and the scheme is additive if one is ever wanted.

*The known mixed demand, recorded rather than waved away.* The decline is **not** "no consumer wants a mixed order". DEFLATE — the [0069](0069-compression.md) consumer this proposal cites for the LSB-first family — packs its non-Huffman elements LSB-first but packs **Huffman codes most-significant-bit-of-code first** into that same LSB-first stream; `ReadBitsLittleEndianLsbFirst(n)` on a Huffman code returns it bit-reversed. Nothing is unbuildable: the answer is the per-bit loop, `ReadBitLsbFirst()` accumulated MSB-first by the caller (`code := (code << 1) | (bit ? 1 : 0)`), which is what a Huffman decoder walks anyway, one bit at a time against a length table. So the mixed family would save a decoder no loop it does not already have — which is the honest ground for declining it, and the ground on which to reopen if a consumer appears whose mixed field is read in one shot.

*Bit numbering, normatively.* Let *k* be the absolute bit index `BitStart + i` for view bit *i*. Under **MsbFirst**, bit *k* is bit `7 - (k mod 8)` of byte `k / 8` and the first bit read is the **most** significant bit of the result; under **LsbFirst**, bit *k* is bit `k mod 8` of byte `k / 8` and the first bit read is the **least** significant. The agreement law follows from these two sentences.

### D4 — Result type: one `ulong` for every width 1–64

Every multi-bit read returns `ulong`; every multi-bit write takes `ulong`; `count` ranges over 1–64.

*Not the smallest containing primitive.* That tier cannot be selected by the width, the width being a *parameter* here rather than part of the name as in §4.14. It would have to be four separately-named families — quadrupling the member count — each with a runtime `count`-versus-type check the single-`ulong` form does not need. And the call-site cast is hazard-free: Bezel's narrowing casts are checked (§5.2 — an unrepresentable value raises `OverflowException`), and a `count`-bit value is representable in every primitive of at least `count` bits **by construction**, so `ushort(r.ReadBitsBigEndianMsbFirst(16))` is total. The narrower tier buys notation only — the standard the rationale already applies to this class of addition (rationale §18 on tuples: *"an **ergonomic convenience, not a capability gap**"*).

*Signedness.* No signed form: a two's-complement field is one sign-extension expression on the `ulong`, and a signed twin would double the family for a case no queued consumer has.

### D5 — Faults: the §4.14 model, and no contract clauses

Reads raise `IndexOutOfRangeException` when the view holds fewer than `count` bits — stdlib §4.14's rule (*"Read methods throw `IndexOutOfRangeException` if the span is shorter than the type being read"*) at bit granularity. A `count` outside 1–64 raises `ArgumentOutOfRangeException`, and `AlignedTail()` on a non-aligned view raises `InvalidOperationException` — both band 1 of stdlib §1's failure-signal rule (caller bug), matching stdlib §1's defined-fault stance for magnitudes.

**No `requires` clauses are proposed**, though band 1 names them first (*"a `requires` clause where a contract is possible"*). One of the two reasons for that has just expired and is not being leaned on: 0106 slate item **iii** has **landed** (v0.263), and stdlib §9.8 now marks `Range<T>.Contains` `pure` with this proposal's own example shape — *"The marker is what admits a range test inside a contract clause: `requires(port in 0..65535)`"* — so `requires(count in 1..64)` is now spellable. The surviving reason stands alone: no member declaration in `Bezel_Standard_Library.md` carries a contract clause today (verified zero at v0.263), and setting that library-wide precedent sideways, inside a small accessor proposal, is the wrong vehicle for it. Recorded so the omission is a decision, not an oversight — and if the owner wants the library's first contract clauses, this surface is a clean place to start and the change is three clauses.

### D6 — Try twins per stdlib §1

`TryRead*` is **shape (c)** — *"`bool` + non-nullable value-typed out"*, `default(T)` on the false path — which stdlib §1 names as the shape *"the §4.14/§4.15 `TryRead*` accessors"* already use. `TryWrite*` is **shape (d)**, bool-only, *"whose payload, if any, was passed **in**"* — §4.14's `TryWrite*` shape, inheriting its atomicity guarantee: on the false path the destination is **not modified**.

One shape-(c) wrinkle, noted rather than smoothed: `TryReadBitMsbFirst(out value: bool): bool` assigns `default(bool)` on the false path, which is indistinguishable from a legitimately-read `false` bit. That is exactly what stdlib §1 sanctions for shape (c) — *"a real value distinguished only by the `bool`"* — so it is conforming, but it reads poorly enough to be worth a sentence at the surface: callers branch on the return, never on the out.

### D7 — The proposed surface

```bezel
// ▲ New. System.Collections.Generic (stdlib §4), beside the §4.14 span family.
public record BitReader(Source: ReadOnlySpan<byte>, BitStart: int, BitLength: int)

  public pure property IsEmpty: bool => BitLength = 0;
  public pure property IsByteAligned: bool => BitStart mod 8 = 0;

  // Reads — from bit 0 of the view (§4.14's no-offset rule, at bit granularity)
  public method ReadBitsBigEndianMsbFirst(count: int): ulong;      // count in 1..64
  public method ReadBitsLittleEndianLsbFirst(count: int): ulong;   // count in 1..64
  public method ReadBitMsbFirst(): bool;
  public method ReadBitLsbFirst(): bool;

  // Try twins — stdlib §1 shape (c)
  public method TryReadBitsBigEndianMsbFirst(count: int, out value: ulong): bool;
  public method TryReadBitsLittleEndianLsbFirst(count: int, out value: ulong): bool;
  public method TryReadBitMsbFirst(out value: bool): bool;
  public method TryReadBitLsbFirst(out value: bool): bool;

  // Repositioning — the §4.14 Slice family, counted in bits
  public method Slice(bitStart: int): BitReader;
  public method Slice(bitStart: int, bitLength: int): BitReader;
  public method Slice(r: Range<int>): BitReader;      // §4.14's v0.241 normalization contract
  public method AlignToByte(): BitReader;             // advance to the next byte boundary

  // The byte-aligned tail — an ordinary sub-span, no bit machinery
  public method AlignedTail(): ReadOnlySpan<byte>;
end BitReader;

public record BitWriter(Source: Span<byte>, BitStart: int, BitLength: int)

  public pure property IsEmpty: bool => BitLength = 0;
  public pure property IsByteAligned: bool => BitStart mod 8 = 0;

  public method WriteBitsBigEndianMsbFirst(value: ulong, count: int);
  public method WriteBitsLittleEndianLsbFirst(value: ulong, count: int);
  public method WriteBitMsbFirst(value: bool);
  public method WriteBitLsbFirst(value: bool);

  // Try twins — stdlib §1 shape (d); on false the destination is not modified
  public method TryWriteBitsBigEndianMsbFirst(value: ulong, count: int): bool;
  public method TryWriteBitsLittleEndianLsbFirst(value: ulong, count: int): bool;
  public method TryWriteBitMsbFirst(value: bool): bool;
  public method TryWriteBitLsbFirst(value: bool): bool;

  public method Slice(bitStart: int): BitWriter;
  public method Slice(bitStart: int, bitLength: int): BitWriter;
  public method Slice(r: Range<int>): BitWriter;
  public method AlignToByte(): BitWriter;

  public method AlignedTail(): Span<byte>;
  public method AsReadOnly(): BitReader;              // parallels Span<T>.AsReadOnly (§4.14)
end BitWriter;

// ▲ Entry points, parallel to AsSpan / AsReadOnlySpan (§4.14)
public extension ReadOnlySpan<byte>
  public method AsBitReader(): BitReader;
end;

public extension Span<byte>
  public method AsBitReader(): BitReader;             // delegates via AsReadOnly()
  public method AsBitWriter(): BitWriter;
end;
```

A partial write leaves the untouched bits of a boundary byte intact (read-modify-write of the two edge bytes). `AlignedTail()` returns the whole-byte remainder `Source.Slice(BitStart / 8, BitLength / 8)`; a trailing partial byte (`BitLength mod 8 <> 0`) is **excluded**, documented rather than silently folded in.

**The §4.15 mirror.** ▲`ArrayBitReader` and ▲`ArrayBitWriter` mirror the two records onto the `Array<byte>`-backed family — member-for-member identical, `ReadOnlyArraySpan<byte>` / `ArraySpan<byte>` substituted for the source type — with ▲`AsBitReader` / ▲`AsBitWriter` entry points on those spans. This is stdlib §4.15's own device for the byte tier (*"the §4.14 endianness-aware extension family is mirrored onto this family … with identical semantics"*), and it is not decoration: the families are **not interchangeable** (stdlib §4.15: *"a method that takes `Span<T>` does not accept `ArraySpan<T>` and vice versa"*), and the two dominant byte sources sit on opposite sides — `Stream.Read(buffer: Span<byte>)` (stdlib §7.1) yields the buffer-backed family, while `File.ReadAllBytes` and `Stream.ReadAll()` return `owned Array<byte>` (stdlib §7.3/§7.1), whose `AsReadOnlySpan()` is the array-backed one.

**Borrow story — zero new machinery.** ▲`BitReader` is borrow-bearing by **transitivity**, under §9.7's third bullet: a field is borrow-carrying when *"Its type is another borrow-bearing record, or a borrow-bearing choice (§35.4) (transitivity)."* Its `Source` is a `ReadOnlySpan<byte>`, which stdlib §4.16 confirms is borrow-bearing; stdlib §4.15's rule then applies unchanged, with "reader" for "span": *"The `Source` field carries a borrow of the array; per the borrow-bearing-record rule, the span's lifetime is bounded by the source's lifetime."* All consequences are inherited: no class instance field, no static field, no escaping capture, no construction from a temporary (§9.7); no `async` parameter or result and constrained across `await` (§30.1 — the bar that removed the async span overloads at stdlib §7.1, v0.179). **No ownership interaction whatsoever**: records cannot have `owned` fields or destructors (§9.7), nothing transfers, disposal is untouched.

---

## 3. What is **not** proposed

- **Bit-syntax patterns.** The pattern-position half is **not** proposed, and no longer needs to be described as forthcoming: it landed as **rationale §11.9** (*"No bit-syntax patterns: sub-byte matching stays out of pattern position (v0.263)"*), which names this proposal by number and makes the same borrow argument this proposal's §1 makes — *"a matched remainder wants to be a sub-view of the scrutinee, which in Bezel is a borrow-bearing span"*, so a pattern element *"would mint a borrow whose source is the scrutinee and whose lifetime pattern arms do not currently reason about — a borrow-producing binder, which no pattern kind is"*. §11.9 is explicit that the decline does not reach here (*"None of that reaches the *reader*"*, which it routes to *"its own proposal (0109), where it must be settled on stdlib grounds"*). Its **reopening bar**: *"a design that answers all three — a binding form whose type is not computed inside the pattern, an exhaustiveness account for width alternatives, and a borrow-producing binder with a stated lifetime rule — not a demonstration that bit matching reads nicely."* Nothing proposed here adds a pattern kind, a binder, or an exhaustiveness obligation, so nothing here moves that bar in either direction.
- **A heap `BitArray`.** [Proposal 0025](0025-bitarray.md) owns that surface. **The seam:** 0025 is a bit **set** — an owning, mutable *class* over its own packed storage, with indexed `bool` access and bulk `And`/`Or`/`Xor`/`Not`; this is a bit **view** — a non-owning value record over bytes somebody else owns, whose operation is field extraction at a position. Neither substitutes for the other (0106 §10: *"0025's `BitArray` is adjacent to but not a substitute for item v"*), and they do not interact: no member here takes or returns a `BitArray`.
- **A parse-and-carry byte-size type.** 0106 §9's low-priority candidate (the `TimeSpan`-shaped size type for config work) is out of scope here.

---

## 4. Costs

**Stdlib surface growth — the only non-zero cost.** Counted exactly: **4 new type names** (▲`BitReader`, ▲`BitWriter`, ▲`ArrayBitReader`, ▲`ArrayBitWriter`), **62 members** (15 per reader, 16 per writer, ×2 families) and **6 extension entry points** — **68 members**, plus 3 positional components per record whose read-only properties are auto-generated (§9.4). Calibration against the tier it extends: stdlib §4.14's byte accessors are 32 members on `ReadOnlySpan<byte>` plus 64 on `Span<byte>` (Read/TryRead/Write/TryWrite, 16 each) = 96, mirrored onto §4.15 for **192**. The bit tier is roughly a **third** of the byte tier it generalizes, because the width is a parameter here rather than part of every name.

The three other axes are **verified zeros**:

- **No language change.** Every construct is landed: positional records (§9.2), value receivers as borrows (§9.4), borrow-bearing records and transitivity (§9.7, §3.6), extensions on a constructed type (stdlib §4.14 already declares `public extension ReadOnlySpan<byte>`), `out` parameters (§22.3), checked casts (§5.2). Appendix A is untouched.
- **No compiler-known status.** stdlib §4.15 says of the span records *"purely user-level Bezel — no compiler intrinsic. The record machinery, borrow lifetime tracking, and indexer dispatch are all standard language features"*; that holds verbatim for these four. The implementation is mask-and-shift over borrowed bytes — the ladder the consumers write today, written once.
- **No new §3.6 derived type property, and no new verdict under any existing one.** §3.6's table carries **eleven** properties at v0.263, and the four new records are classified by every one of them without a rule being touched. Three do not apply: *trivial-dispose* (classes only), *choice classification* and *holds-borrows* (choices only). Four take the same verdict the `Source` span already takes, by the same computation on the same field: **borrow-bearing — yes**, by §9.7's transitivity bullet; **sendable — no** (§30.14: *"Value types are sendable iff they are not borrow-bearing … and not move-only"*), which is correct and wanted, a bit view being a window onto another domain's memory; **auto-hashable / hashable — no**, because §9.2's admissible-component list admits no class-typed component and the span's `Buffer`/`Source` field is one, so the record is neither auto-hashable nor declares user equality (§3.6's *hashable* exclusion (a)) — no member of the proposed surface depends on equality or hashing; **defaultable — no** (§17.11: a record is defaultable only if every declared field is, and a non-nullable class field is not), so `default(BitReader)` cannot fabricate a source-less view. Two more resolve the same way as the spans: *serialization-eligible* — no, by the same class-typed component; *C-compatible* — no, stdlib §3.4's reason verbatim, *"a borrow-bearing record has no C-compatible form"*. The eleventh, *element category*, classifies these as **value** elements, so a collection slot holding one is a **copy** slot carrying §9.7's outlives obligation (§28.11), exactly as a slot holding a span does.

**Two costs that are not surface**, named so they are not discovered later. First, D2's width-stated-twice footgun (Q3). Second, an `async` restriction that is narrower than "synchronous-only": a bit view may **not** be an async method's parameter or result (§30.1 — the bar that removed the async span overloads, stdlib §7.1's v0.179 note), but stdlib §4.14 states that a span *local* whose source is co-resident in the same async frame *"(an owned local, `using var`, or `owned` parameter of that method)"* **may** live *"across `await` points in that method"*, and a bit view over such a span inherits that. So an async framing loop that owns its buffer keeps its reader across awaits; what it cannot do is *pass* one to an async callee — that boundary takes the `Array<byte>` windowed form, as stdlib §7.1 directs.

---

## 5. Interactions

- **[0025](0025-bitarray.md) (`BitArray`, under review).** Adjacent, not overlapping; the seam is stated in this proposal's §3. Neither ordering constrains the other.
- **[0027](0027-splitspans.md) (`SplitSpans`, accepted).** The precedent followed *and* departed from: its D1/D2 establish that a class cursor may not carry a span borrow (D1's argument here), but its escape — emit `Range<int>`, re-derive the span — is unavailable at bit granularity. Both end in the same place: a cursor that is not an `IEnumerable` (§29.2).
- **[0009](0009-base64-hex.md) (Base64 + hex, accepted, not yet landed).** The one-shot conventions deliberately *not* copied: 0009 D5 takes `IReadOnlySpan<byte>` in and returns `owned Array<byte>` out, right for a whole-buffer transform; a positional view cannot take the interface at all (D1) and allocates nothing. Where they meet — hex of a bit-extracted field — the composition is `AlignedTail()` into 0009's span-taking encode.
- **stdlib §4.14 / §4.15.** The direct parent: a tier *below* the byte accessors, inheriting four rules unchanged (endianness in the name, no offset parameter, the `IndexOutOfRangeException` bounds model, the Try twins) plus the §4.15 mirroring device. D3's agreement law keeps the tiers from drifting.
- **[0007](0007-system-net-substrate.md) (`System.Net` substrate, accepted, wave D1) and [0001](0001-networking-http.md).** The substrate consumer: `TcpConnection`/`NetworkStream` deliver the bytes; every framed protocol above them (DNS, HTTP/2 frame headers, TLS records at the sans-IO seam) reads sub-byte fields out of them. The largest first customer, and why 0106 rated the item **High** on scope. **[0069](0069-compression.md)** motivates the LSB-first family (DEFLATE-shaped packing); **[0029](0029-checksums.md)** / **[0020](0020-regex.md)** read framing bits at their edges.
- **0106 slate item iii — landed (v0.263).** `Range<T>.Contains` is now `pure` (stdlib §9.8), so `requires(count in 1..64)` is spellable and no longer blocks anything here. D5 keeps the defined-fault spelling on the surviving reason only — the stdlib carries no contract clauses at all — and names the three clauses an owner could ask for instead.
- **rationale §11.9 (v0.263).** The pattern-position decline this proposal's §3 leans on; it names 0109 as the home of the reader half, so the two documents interlock and §11.9's reopening bar is the one to check before any bit-pattern question is re-asked.

---

## 6. Questions for the owner

- **Q1 — Result type (D4): one `ulong`, or a smallest-containing-primitive tier?** Recommendation: one `ulong` — the checked cast is total for a correct width, so the narrower tier is notation at 4× the member count. Honest counter: `ushort(r.ReadBitsBigEndianMsbFirst(16))` puts a cast on every call site of a *reading* API, which §4.14's typed names deliberately avoid — the one place this design reads worse than its parent.
- **Q2 — The Msb/Lsb surface (D3): two compound-named families, four, or one axis?** Recommendation: two, because those two alone reduce to existing §4.14 accessors at byte-aligned widths. Confirmation is wanted specifically on *declining the two mixed combinations*, which this proposal would record as a decline-with-reasons rather than leave silent.
- **Q3 — The advance shape (D2), the sharpest question, with four options and one language bar.** What §9.4 forecloses is exactly one of them: a cursor stored in the receiver's own storage. The rest is design judgment, so the owner is ruling on judgment, not on a rule.
  **(a) Slice-and-walk** *(recommended, per 0106 §3.5's own correction)* — `v := r.Read…(16); r := r.Slice(16);`. Allocation-free, no convention broken, peek is free; cost: each width is stated twice and a mismatch is silent corruption.
  **(b) Fused via `out`** — `r := r.Read…(16, out id)`. States each width once; inverts stdlib §1's `out`-is-the-failure-channel convention, and every read becomes an assignment to the cursor.
  **(c) Mediated heap cursor** (a self-advancing reader whose position lives in a borrowed heap cell) — **legal** under §9.4's mediating-hop rule, not barred; declined for allocation per view, a cursor shared by every copy of a value type, and a record whose value is not its state.
  **(d) Named-record fused return** — ▲`record BitRead(Value: ulong, Rest: BitReader)`, the shape rationale §18's product doctrine points at. States each width once, breaks no convention, stores nothing in the receiver; costs two extra ▲ types (the writer needs its own), a destructuring binding at every call site, and an awkward `Try*` twin (the false path would have to carry an invalid `Rest`).
  A ruling for (b) or (d) changes 8 signatures and no other part of this proposal; (c) additionally changes the record's field list and its copy semantics.
- **Q4 — Does the §4.15 mirror land in the same batch?** Recommendation: yes — mechanical and member-identical, and without it either the socket path or the file path keeps hand-rolling ladders. Counter: half the member count for a family with no field evidence yet, so the §4.14 pair alone is a defensible smaller first step.

---

*Proposed README row (Live queue):*

| [0109](0109-bit-reader-writer.md) | [`BitReader`/`BitWriter`](0109-bit-reader-writer.md) — bit-granular views over byte spans: borrow-bearing value records with `(BitStart, BitLength)` windows, 1–64 bits into `ulong`, endianness **and** bit order in the name (the two combinations that reduce to §4.14 at byte-aligned widths), Try twins per §1, slice-and-walk advance (§9.4 bars a cursor field of the receiver; the mediated cursor is legal and declined on design grounds — Q3 puts all four shapes to the owner), mirrored onto §4.15; 68 members, no language change; pattern-position half already declined at rationale §11.9 (v0.263), `BitArray` left to 0025 (0106 slate item v) | under-review — draft (2026-08-20) | owner ruling on Q1–Q4 | — |
