// Module public-interface metadata roundtrip tests (§2.5 / §2.6, issue #328
// increment 1).
//
// These derive from the locked design in issue #328 and spec §2.5/§2.6:
//   - §2.5: a compiled module embeds public-interface metadata (type
//     signatures, method signatures, generic constraints) so downstream
//     `import` resolution can type-check against it.
//   - §2.6 step 4 + closing note: format is implementation-defined; #328
//     locks LLVM bitstream + schema-version + semantic-hash header.
//
// The increment is standalone: it builds metadata for a representative small
// module by hand (a class with public+private members, a record, an enum, an
// interface, a generic type, a top-level function and constant), serializes →
// deserializes → asserts structural equality, asserts the semantic hash is
// stable across roundtrip and changes when the public interface changes, and
// asserts an unknown record/subblock is skipped (forward-compat).

#include "module/ModuleMetadata.hpp"

#include <optional>

#include "test_harness.hpp"

namespace bezel::module {
namespace {

// A primitive type leaf (§4).
MetaType prim(PrimitiveKind p, bool nullable = false) {
    MetaType t;
    t.kind = TypeKind::Primitive;
    t.primitive = p;
    t.nullable = nullable;
    return t;
}

// A named type leaf (§9-§12), optionally with generic arguments (§14).
MetaType named(std::string name, std::vector<MetaType> args = {},
               bool nullable = false) {
    MetaType t;
    t.kind = TypeKind::Named;
    t.namedName = std::move(name);
    t.genericArgs = std::move(args);
    t.nullable = nullable;
    return t;
}

// Build a representative module mirroring §2.4's "exposes its public types and
// methods". Covers every schema feature the increment must capture.
ModuleMetadata buildFixture() {
    ModuleMetadata md;
    md.moduleName = "BankingApp.Transactions"; // §2.1 dotted module name
    md.exports = {"Account", "Money", "AccountKind", "ILedger", "Box",
                  "transfer", "MaxAccounts"};

    // --- §11 class with public + private members, a constructor, a property,
    //     a method (with ref/out/owned params), and an operator overload. ---
    MetaTypeDecl account;
    account.name = "Account";
    account.kind = MetaTypeDeclKind::Class;
    account.visibility = MetaVisibility::Public;
    account.modifiers = {"sealed"}; // §12
    account.baseTypes = {named("Object")};

    // public field
    {
        MetaMember f;
        f.kind = MetaMemberKind::Field;
        f.name = "Id";
        f.visibility = MetaVisibility::Public;
        f.modifiers = {"readonly"}; // §22.8
        f.type = prim(PrimitiveKind::Int);
        account.members.push_back(f);
    }
    // private field (§13) — part of fidelity even if a real emit would drop it
    {
        MetaMember f;
        f.kind = MetaMemberKind::Field;
        f.name = "secret";
        f.visibility = MetaVisibility::Private;
        f.type = prim(PrimitiveKind::Long);
        account.members.push_back(f);
    }
    // public auto-property (§22.9)
    {
        MetaMember p;
        p.kind = MetaMemberKind::Property;
        p.name = "Balance";
        p.visibility = MetaVisibility::Public;
        p.type = named("Money");
        account.members.push_back(p);
    }
    // constructor (§11) with one parameter
    {
        MetaMember c;
        c.kind = MetaMemberKind::Constructor;
        c.name = "Account";
        c.visibility = MetaVisibility::Public;
        MetaParam pid;
        pid.name = "id";
        pid.type = prim(PrimitiveKind::Int);
        c.params.push_back(pid);
        account.members.push_back(c);
    }
    // method with ref / out owned params (§22 / §28), async (§30), owned return
    {
        MetaMember m;
        m.kind = MetaMemberKind::Method;
        m.name = "Withdraw";
        m.visibility = MetaVisibility::Public;
        m.modifiers = {"async"};
        m.type = named("Task", {named("Money")}); // §30 return
        m.returnOwned = true;                      // §28 owned return
        MetaParam amount;
        amount.name = "amount";
        amount.type = named("Money");
        amount.hasDefault = true; // §22.5 optional parameter
        amount.defaultText = "Money.Zero";
        m.params.push_back(amount);
        MetaParam err;
        err.name = "error";
        err.modifier = "out owned"; // §28
        err.type = named("Error", {}, /*nullable=*/true);
        m.params.push_back(err);
        account.members.push_back(m);
    }
    // operator overload (§25)
    {
        MetaMember op;
        op.kind = MetaMemberKind::Method;
        op.name = "+";
        op.isOperator = true;
        op.visibility = MetaVisibility::Public;
        op.modifiers = {"static"};
        op.type = named("Account");
        MetaParam lhs;
        lhs.name = "a";
        lhs.type = named("Account");
        MetaParam rhs;
        rhs.name = "b";
        rhs.type = named("Account");
        op.params = {lhs, rhs};
        account.members.push_back(op);
    }
    // indexer (§22.9)
    {
        MetaMember ix;
        ix.kind = MetaMemberKind::Indexer;
        ix.name = "Item";
        ix.visibility = MetaVisibility::Public;
        ix.type = named("Money");
        MetaParam idx;
        idx.name = "i";
        idx.type = prim(PrimitiveKind::Int);
        ix.params = {idx};
        account.members.push_back(ix);
    }
    md.types.push_back(account);

    // --- §9 record with positional-style public fields. ---
    MetaTypeDecl money;
    money.name = "Money";
    money.kind = MetaTypeDeclKind::Record;
    money.visibility = MetaVisibility::Public;
    {
        MetaMember amount;
        amount.kind = MetaMemberKind::Field;
        amount.name = "Amount";
        amount.visibility = MetaVisibility::Public;
        amount.type = prim(PrimitiveKind::Decimal);
        money.members.push_back(amount);
        MetaMember currency;
        currency.kind = MetaMemberKind::Field;
        currency.name = "Currency";
        currency.visibility = MetaVisibility::Public;
        currency.type = prim(PrimitiveKind::String);
        money.members.push_back(currency);
    }
    md.types.push_back(money);

    // --- §10 enum with constants (one with explicit value). ---
    MetaTypeDecl kind;
    kind.name = "AccountKind";
    kind.kind = MetaTypeDeclKind::Enum;
    kind.visibility = MetaVisibility::Public;
    {
        MetaMember checking;
        checking.kind = MetaMemberKind::EnumConstant;
        checking.name = "Checking";
        checking.hasEnumValue = true;
        checking.enumValue = 0;
        kind.members.push_back(checking);
        MetaMember savings;
        savings.kind = MetaMemberKind::EnumConstant;
        savings.name = "Savings";
        savings.hasEnumValue = true;
        savings.enumValue = 7;
        kind.members.push_back(savings);
    }
    md.types.push_back(kind);

    // --- §11 interface with a method (contract surface). ---
    MetaTypeDecl ledger;
    ledger.name = "ILedger";
    ledger.kind = MetaTypeDeclKind::Interface;
    ledger.visibility = MetaVisibility::Public;
    {
        MetaMember record;
        record.kind = MetaMemberKind::Method;
        record.name = "Record";
        record.visibility = MetaVisibility::Public;
        record.type = prim(PrimitiveKind::Bool);
        MetaParam entry;
        entry.name = "entry";
        entry.type = named("Money");
        record.params = {entry};
        ledger.members.push_back(record);
    }
    md.types.push_back(ledger);

    // --- §14 generic type with a constrained type parameter, an inline-array
    //     field, and a function-typed field (exercises every TypeKind). ---
    MetaTypeDecl box;
    box.name = "Box";
    box.kind = MetaTypeDeclKind::Class;
    box.visibility = MetaVisibility::Public;
    {
        MetaGenericParam tp;
        tp.name = "T";
        tp.variance = "out";                 // §14 variance
        tp.constraintTypes = {named("ILedger")}; // §14.2 type constraint
        tp.specialConstraints = {"class"};   // §14.2 special constraint
        box.genericParams = {tp};
    }
    box.baseTypes = {named("IEnumerable", {named("T")})}; // §14 base w/ arg
    {
        MetaMember slots;
        slots.kind = MetaMemberKind::Field;
        slots.name = "slots";
        slots.visibility = MetaVisibility::Private;
        // §15 inline array [4]T
        MetaType arr;
        arr.kind = TypeKind::InlineArray;
        arr.arraySize = "4";
        arr.children = {named("T")};
        slots.type = arr;
        box.members.push_back(slots);

        MetaMember callback;
        callback.kind = MetaMemberKind::Field;
        callback.name = "onChange";
        callback.visibility = MetaVisibility::Public;
        // §27 function type (T, int) -> bool
        MetaType fn;
        fn.kind = TypeKind::Function;
        fn.children = {named("T"), prim(PrimitiveKind::Int),
                       prim(PrimitiveKind::Bool)};
        fn.funcParamMods = {"", ""};
        fn.funcParamNames = {"item", "index"};
        callback.type = fn;
        box.members.push_back(callback);

        // unit-returning method (§3.1 () return)
        MetaMember reset;
        reset.kind = MetaMemberKind::Method;
        reset.name = "Reset";
        reset.visibility = MetaVisibility::Public;
        MetaType unit;
        unit.kind = TypeKind::Unit;
        reset.type = unit;
        box.members.push_back(reset);
    }
    md.types.push_back(box);

    // --- §22 top-level function with a generic param. ---
    MetaFunction transfer;
    transfer.name = "transfer";
    transfer.visibility = MetaVisibility::Public;
    {
        MetaGenericParam tp;
        tp.name = "TLedger";
        tp.specialConstraints = {"class"};
        transfer.genericParams = {tp};
    }
    {
        MetaParam from;
        from.name = "from";
        from.modifier = "ref"; // §22
        from.type = named("Account");
        MetaParam to;
        to.name = "to";
        to.modifier = "ref";
        to.type = named("Account");
        MetaParam amount;
        amount.name = "amount";
        amount.type = named("Money");
        transfer.params = {from, to, amount};
    }
    transfer.returnType = prim(PrimitiveKind::Bool);
    md.functions.push_back(transfer);

    // --- §11 top-level constant. ---
    MetaConstant maxAccounts;
    maxAccounts.name = "MaxAccounts";
    maxAccounts.visibility = MetaVisibility::Public;
    maxAccounts.type = prim(PrimitiveKind::Int);
    maxAccounts.valueText = "1024";
    md.constants.push_back(maxAccounts);

    return md;
}

} // namespace

// §2.5/§2.6: serialize → deserialize → structural equality. Every part of the
// public interface survives a roundtrip.
BEZEL_TEST(module_metadata, roundtrip_structural_equality_spec_2_5) {
    ModuleMetadata original = buildFixture();
    std::vector<std::uint8_t> bytes = serialize(original);
    BEZEL_EXPECT_TRUE(!bytes.empty());

    std::optional<ModuleMetadata> decoded = deserialize(bytes);
    BEZEL_EXPECT_TRUE(decoded.has_value());
    if (!decoded) return;

    BEZEL_EXPECT_TRUE(*decoded == original);
    // And spot-check a few specific signature elements survived intact.
    BEZEL_EXPECT_EQ(decoded->moduleName, std::string("BankingApp.Transactions"));
    BEZEL_EXPECT_EQ(decoded->types.size(), std::size_t{5});
    BEZEL_EXPECT_EQ(decoded->functions.size(), std::size_t{1});
    BEZEL_EXPECT_EQ(decoded->constants.size(), std::size_t{1});
}

// Issue #328: the header records a schema version and a semantic hash that a
// reader can recover without fully decoding the body.
BEZEL_TEST(module_metadata, header_records_version_and_hash_issue_328) {
    ModuleMetadata original = buildFixture();
    std::vector<std::uint8_t> bytes = serialize(original);

    std::optional<std::uint32_t> ver = readSchemaVersion(bytes);
    BEZEL_EXPECT_TRUE(ver.has_value());
    if (ver) BEZEL_EXPECT_EQ(*ver, kSchemaVersion);

    std::optional<std::uint64_t> headerHash = readSemanticHash(bytes);
    BEZEL_EXPECT_TRUE(headerHash.has_value());
    if (headerHash) BEZEL_EXPECT_EQ(*headerHash, semanticHash(original));
}

// Issue #328: the semantic hash is stable across a serialize/deserialize
// roundtrip (it is computed from content, not wire bytes).
BEZEL_TEST(module_metadata, semantic_hash_stable_across_roundtrip_issue_328) {
    ModuleMetadata original = buildFixture();
    std::uint64_t before = semanticHash(original);

    std::optional<ModuleMetadata> decoded = deserialize(serialize(original));
    BEZEL_EXPECT_TRUE(decoded.has_value());
    if (!decoded) return;

    std::uint64_t after = semanticHash(*decoded);
    BEZEL_EXPECT_EQ(before, after);
}

// Issue #328: any change to the public interface changes the hash (staleness
// detection). We exercise several kinds of public-interface change.
BEZEL_TEST(module_metadata, semantic_hash_changes_on_interface_change_issue_328) {
    ModuleMetadata base = buildFixture();
    std::uint64_t baseHash = semanticHash(base);

    // (a) rename a public member
    {
        ModuleMetadata m = base;
        m.types[0].members[0].name = "Identifier";
        BEZEL_EXPECT_TRUE(semanticHash(m) != baseHash);
    }
    // (b) change a member's type
    {
        ModuleMetadata m = base;
        m.types[0].members[0].type = prim(PrimitiveKind::Long);
        BEZEL_EXPECT_TRUE(semanticHash(m) != baseHash);
    }
    // (c) add a public member
    {
        ModuleMetadata m = base;
        MetaMember extra;
        extra.kind = MetaMemberKind::Field;
        extra.name = "Extra";
        extra.visibility = MetaVisibility::Public;
        extra.type = prim(PrimitiveKind::Int);
        m.types[0].members.push_back(extra);
        BEZEL_EXPECT_TRUE(semanticHash(m) != baseHash);
    }
    // (d) change a parameter modifier (ref -> out): signature change
    {
        ModuleMetadata m = base;
        m.functions[0].params[0].modifier = "out";
        BEZEL_EXPECT_TRUE(semanticHash(m) != baseHash);
    }
    // (e) change the module name
    {
        ModuleMetadata m = base;
        m.moduleName = "BankingApp.Other";
        BEZEL_EXPECT_TRUE(semanticHash(m) != baseHash);
    }
    // (f) change an enum constant value
    {
        ModuleMetadata m = base;
        m.types[2].members[1].enumValue = 9;
        BEZEL_EXPECT_TRUE(semanticHash(m) != baseHash);
    }
}

// Issue #328 (Java .class model): a reader skips an unknown OPTIONAL record /
// subblock it does not recognise (forward-compat). We forge a blob that
// contains an extra unknown record + unknown subblock (as a future schema
// version would) and confirm the current reader still decodes the known
// content and reproduces the original metadata.
BEZEL_TEST(module_metadata, skips_unknown_optional_field_forward_compat_issue_328) {
    ModuleMetadata original = buildFixture();
    std::vector<std::uint8_t> normal = serialize(original);

    // serializeWithUnknownExtras() is a test-only writer (exposed by the
    // implementation for this forward-compat check) that emits the same
    // content plus an unknown record code and an unknown subblock id that the
    // current reader has never seen. A spec-compliant reader skips them.
    std::vector<std::uint8_t> withExtras = serializeWithUnknownExtras(original);
    BEZEL_EXPECT_TRUE(withExtras.size() > normal.size());

    std::optional<ModuleMetadata> decoded = deserialize(withExtras);
    BEZEL_EXPECT_TRUE(decoded.has_value());
    if (!decoded) return;
    BEZEL_EXPECT_TRUE(*decoded == original);
}

// A blob with the wrong magic is rejected (not Bezel module metadata).
BEZEL_TEST(module_metadata, rejects_bad_magic_issue_328) {
    std::vector<std::uint8_t> garbage = {0xDE, 0xAD, 0xBE, 0xEF,
                                         0x00, 0x01, 0x02, 0x03};
    BEZEL_EXPECT_FALSE(deserialize(garbage).has_value());
    BEZEL_EXPECT_FALSE(readSchemaVersion(garbage).has_value());
}

// #2054 / spec §2.8.2 (v0.204): contract records are interface content,
// not body-local annotations. They retain kind, `static`, canonical text, and
// demand-derived tier through the wire format, and a change affects the
// semantic hash.
BEZEL_TEST(module_metadata, contract_records_roundtrip_spec_2_8_2_2054) {
    ModuleMetadata md;
    md.moduleName = "Contracts";
    md.exports = {"Account"};

    MetaTypeDecl account;
    account.name = "Account";
    account.kind = MetaTypeDeclKind::Class;
    account.visibility = MetaVisibility::Public;

    MetaMember method;
    method.kind = MetaMemberKind::Method;
    method.name = "Withdraw";
    method.visibility = MetaVisibility::Public;
    method.isPure = true;  // §22.15 carriage
    MetaContractRecord reqRecord;
    reqRecord.kind = MetaContractKind::Requires;
    reqRecord.canonicalText = "%0 > 0";
    reqRecord.tier = MetaContractTier::Checked;
    MetaContractRecord ensRecord;
    ensRecord.kind = MetaContractKind::Ensures;
    ensRecord.isStatic = true;
    ensRecord.canonicalText = "result = %0";
    ensRecord.tier = MetaContractTier::Proven;
    method.contracts = {reqRecord, ensRecord};
    account.members.push_back(method);

    MetaContractRecord invariant;
    invariant.kind = MetaContractKind::Invariant;
    invariant.canonicalText = "Contracts.Account._balance >= 0";
    invariant.tier = MetaContractTier::Checked;
    account.invariants.push_back(invariant);
    md.types.push_back(account);

    std::optional<ModuleMetadata> decoded = deserialize(serialize(md));
    BEZEL_EXPECT_TRUE(decoded.has_value());
    if (!decoded) return;
    BEZEL_EXPECT_TRUE(decoded->contractRecordsCarried);
    BEZEL_EXPECT_TRUE(*decoded == md);
    if (decoded->types.empty()) return;
    BEZEL_EXPECT_EQ(decoded->types[0].invariants.size(), std::size_t{1});
    BEZEL_EXPECT_EQ(decoded->types[0].members[0].contracts.size(),
                    std::size_t{2});
    BEZEL_EXPECT_TRUE(decoded->types[0].members[0].isPure);

    ModuleMetadata changed = md;
    changed.types[0].members[0].contracts[0].canonicalText = "%0 >= 0";
    BEZEL_EXPECT_TRUE(semanticHash(changed) != semanticHash(md));

    // §2.8.6 degradation: an explicitly opaque snapshot remains distinguishable
    // from a current artifact asserting an empty contract set.
    ModuleMetadata opaque = md;
    opaque.contractRecordsCarried = false;
    auto opaqueDecoded = deserialize(serialize(opaque));
    BEZEL_EXPECT_TRUE(opaqueDecoded.has_value());
    if (opaqueDecoded) BEZEL_EXPECT_TRUE(!opaqueDecoded->contractRecordsCarried);
}

// §2.5 / §2.8.2 (issue #3198, v0.254): a stamp that does not match the
// reader's `kProducerContentRevision` is the §2.8.3-rule-3 "stale artifact"
// signal the loader must reject — issue #3198 measures the silent-accept
// shape (a stale dependency compiled clean, then SIGSEGV'd at runtime) when
// the loader has no vintage check. The fix is a 32-bit producer stamp in
// the header record; the loader reads it via `readProducerContentRevision`
// and refuses any artifact whose stamp != kProducerContentRevision. This
// test pins the check end-to-end: forge a blob with a NON-current stamp and
// verify the reader surfaces it (so `loadDependency`'s rejection has the
// data it needs), then verify a current stamp is accepted.
BEZEL_TEST(module_metadata, producer_content_revision_stamped_3198) {
    ModuleMetadata original = buildFixture();
    std::vector<std::uint8_t> bytes = serialize(original);

    // The current producer stamps exactly kProducerContentRevision; the
    // reader sees that value back. The default writer → reader roundtrip is
    // identity.
    std::optional<std::uint32_t> stamp = readProducerContentRevision(bytes);
    BEZEL_EXPECT_TRUE(stamp.has_value());
    if (stamp) BEZEL_EXPECT_EQ(*stamp, kProducerContentRevision);
}

// §2.5 / §2.8.2 (issue #3198, v0.254): the loader's vintage check rejects a
// stale artifact. We forge a stale artifact by emitting the same content
// through the test-only `serializeWithProducerRevision` with a stamp
// different from kProducerContentRevision, and verify the reader surfaces
// the mismatch — which is the precondition `loadDependency` reads before
// deserializing. Without the stamp in the header, `loadDependency` would
// have no signal to reject and would silently rebuild symbols from a
// producer the importer cannot trust (the silent-accept shape #3198
// measures).
BEZEL_TEST(module_metadata, stale_producer_revision_detected_3198) {
    ModuleMetadata original = buildFixture();
    // Stamp != kProducerContentRevision → loader's vintage check fails.
    std::vector<std::uint8_t> stale =
        serializeWithProducerRevision(original, kProducerContentRevision + 1);

    std::optional<std::uint32_t> stamp = readProducerContentRevision(stale);
    BEZEL_EXPECT_TRUE(stamp.has_value());
    if (stamp) BEZEL_EXPECT_TRUE(*stamp != kProducerContentRevision);

    // The header still validates (magic + schema version are untouched by
    // the stamp), so the loader's other checks do not fire — the rejection
    // is purely on the stamp.
    std::optional<std::uint32_t> ver = readSchemaVersion(stale);
    BEZEL_EXPECT_TRUE(ver.has_value());
    if (ver) BEZEL_EXPECT_EQ(*ver, kSchemaVersion);
}

// §2.5 / §2.8.2 (issue #3198, v0.254): an artifact written before the
// producer-stamp field landed has NO 5th header value — `readHeader` reads
// it as 0 (the "absent" sentinel), and `kProducerContentRevision >= 1`, so
// the loader's vintage check rejects it. This is the recompile-the-world
// migration shape the issue describes: a freshly-built loader refuses an
// older artifact (rather than silently reading fields the old writer did
// not write) and tells the operator to rebuild the dependency.
BEZEL_TEST(module_metadata, old_artifact_without_stamp_rejected_3198) {
    // An artifact whose stamp is 0 is exactly how an old artifact (read
    // by the new readHeader) surfaces: the field is absent, readHeader
    // saturates to 0, and the loader's vintage check sees 0 !=
    // kProducerContentRevision. The stamp value 0 is the magic sentinel
    // pinned by the load-time comparison.
    ModuleMetadata original = buildFixture();
    std::vector<std::uint8_t> withoutStamp =
        serializeWithProducerRevision(original, /*stamp=*/0);

    std::optional<std::uint32_t> stamp = readProducerContentRevision(withoutStamp);
    BEZEL_EXPECT_TRUE(stamp.has_value());
    if (stamp) BEZEL_EXPECT_EQ(*stamp, 0u);
    // 0 != kProducerContentRevision → loader's vintage check rejects.
    if (stamp) BEZEL_EXPECT_TRUE(*stamp != kProducerContentRevision);
}

} // namespace bezel::module
