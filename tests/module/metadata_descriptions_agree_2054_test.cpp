// §2.8.3 rule 3 / §2.8.2 / §2.8.6 (v0.204, #2054): the metadata-time
// staleness backstop. Two ModuleMetadata descriptions for the same module
// agree iff:
//   - both have contractRecordsCarried=true and every field is equal (full
//     record equality), OR
//   - at least one is contract-opaque (a pre-carriage artifact, §2.8.6) and
//     the STRUCTURAL surface — every field except the contract-facts fields
//     (per-member isPure + per-member contracts + per-class invariants +
//     contractRecordsCarried) — is equal.
//
// Each sub-case pins a distinct branch of metadataDescriptionsAgree in
// src/module/ModuleLoader.cpp so the regression survives a future
// refactor that confuses "carried" with "structural" agreement.

#include "module/ModuleLoader.hpp"
#include "module/ModuleMetadata.hpp"

#include <string>
#include <vector>

#include "test_harness.hpp"

namespace bezel::module {
// §2.8.3 rule 3 / #2054: forward-declared for test access. The definition
// lives in src/module/ModuleLoader.cpp (where loadDependencies invokes it
// as the metadata-time staleness backstop). It is not exposed via
// ModuleLoader.hpp because the project keeps the staleness check as an
// internal of the loader; the test pins the four branches here.
bool metadataDescriptionsAgree(const ModuleMetadata& lhs,
                               const ModuleMetadata& rhs);
namespace {

// Local helper: a primitive type leaf (§4). Mirrors the one in
// module_metadata_test.cpp (which lives in an anonymous namespace and so is
// not linkable from this translation unit).
MetaType prim(PrimitiveKind p, bool nullable = false) {
    MetaType t;
    t.kind = TypeKind::Primitive;
    t.primitive = p;
    t.nullable = nullable;
    return t;
}

// Helper: build a small but representative ModuleMetadata with one class,
// one public method carrying isPure=true and one Requires contract.
// Used as the seed for the four sub-cases.
ModuleMetadata seedCarried(const std::string& moduleName,
                           const std::string& contractText) {
    ModuleMetadata md;
    md.moduleName = moduleName;
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
    MetaContractRecord req;
    req.kind = MetaContractKind::Requires;
    req.canonicalText = contractText;
    req.tier = MetaContractTier::Checked;
    method.contracts = {req};
    account.members.push_back(method);
    md.types.push_back(account);

    md.contractRecordsCarried = true;  // current artifact
    return md;
}

// Same structural surface as `seedCarried` but isPure=false, contracts
// empty, contractRecordsCarried=false — the §2.8.6 "opaque" shape. Used
// to verify the structural-equality branch.
ModuleMetadata seedOpaque(const std::string& moduleName) {
    ModuleMetadata md;
    md.moduleName = moduleName;
    md.exports = {"Account"};

    MetaTypeDecl account;
    account.name = "Account";
    account.kind = MetaTypeDeclKind::Class;
    account.visibility = MetaVisibility::Public;

    MetaMember method;
    method.kind = MetaMemberKind::Method;
    method.name = "Withdraw";
    method.visibility = MetaVisibility::Public;
    method.isPure = false;  // opaque
    method.contracts.clear();
    account.members.push_back(method);
    md.types.push_back(account);

    md.contractRecordsCarried = false;  // pre-carriage artifact
    return md;
}

} // namespace

// ---------------------------------------------------------------------------
// Sub-case A: both carried, structurally identical → agree.
// (Both artifacts record the same canonical-text clause.)
// ---------------------------------------------------------------------------
BEZEL_TEST(metadata_descriptions_agree_2054,
           both_carried_equal_agree_spec_2_8_3_rule_3) {
    auto a = seedCarried("Lib", "%0 > 0");
    auto b = seedCarried("Lib", "%0 > 0");
    BEZEL_EXPECT_TRUE(metadataDescriptionsAgree(a, b));
}

// ---------------------------------------------------------------------------
// Sub-case B: both carried, contract record differs → DISAGREE.
// (One artifact records "%0 > 0"; the other records "%0 >= 0". A
//  behavioral difference must fail the agreement check — silent acceptance
//  would mask a stale rebuild and let two callers of the same module see
//  different contracts.)
// ---------------------------------------------------------------------------
BEZEL_TEST(metadata_descriptions_agree_2054,
           both_carried_diff_disagree_spec_2_8_3_rule_3) {
    auto a = seedCarried("Lib", "%0 > 0");
    auto b = seedCarried("Lib", "%0 >= 0");
    BEZEL_EXPECT_FALSE(metadataDescriptionsAgree(a, b));

    // A difference in purity is also a behavioral claim; both carried,
    // differing purity → disagree.
    auto c = seedCarried("Lib", "%0 > 0");
    auto d = c;
    d.types[0].members[0].isPure = false;
    BEZEL_EXPECT_FALSE(metadataDescriptionsAgree(c, d));

    // A difference in invariants (per-class contract records) is also a
    // behavioral claim.
    auto e = seedCarried("Lib", "%0 > 0");
    auto f = e;
    MetaContractRecord inv;
    inv.kind = MetaContractKind::Invariant;
    inv.canonicalText = "Lib.Account._balance >= 0";
    inv.tier = MetaContractTier::Checked;
    f.types[0].invariants.push_back(inv);
    BEZEL_EXPECT_FALSE(metadataDescriptionsAgree(e, f));
}

// ---------------------------------------------------------------------------
// Sub-case C: one opaque, structural surface equal → agree.
// (The opaque artifact makes no behavioral claim; the structural fields
//  (signature types, modifiers, parameter list, return type) match.)
// ---------------------------------------------------------------------------
BEZEL_TEST(metadata_descriptions_agree_2054,
           one_opaque_structural_equal_agree_spec_2_8_6) {
    auto carried = seedCarried("Lib", "%0 > 0");
    auto opaque = seedOpaque("Lib");
    // The carried artifact's structural surface equals the opaque
    // artifact's full surface; the opaque one has no behavioral claims.
    BEZEL_EXPECT_TRUE(metadataDescriptionsAgree(carried, opaque));
    BEZEL_EXPECT_TRUE(metadataDescriptionsAgree(opaque, carried));
}

// ---------------------------------------------------------------------------
// Sub-case D: one opaque, structural surface differs → DISAGREE.
// (The structural surface itself changed — a signature rename, a parameter
//  modifier swap, a class rename. Even with one side opaque, this is a
//  real interface change that the loader must reject.)
// ---------------------------------------------------------------------------
BEZEL_TEST(metadata_descriptions_agree_2054,
           one_opaque_structural_diff_disagree_spec_2_8_6) {
    auto carried = seedCarried("Lib", "%0 > 0");
    auto opaque = seedOpaque("Lib");

    // (1) Method rename — structural field differs.
    {
        auto op = opaque;
        op.types[0].members[0].name = "Take";
        BEZEL_EXPECT_FALSE(metadataDescriptionsAgree(carried, op));
    }
    // (2) Type rename — structural field differs.
    {
        auto op = opaque;
        op.types[0].name = "Acct";
        BEZEL_EXPECT_FALSE(metadataDescriptionsAgree(carried, op));
    }
    // (3) Module-name mismatch is always rejected (the agreement is per
    // module; two descriptions with different names are not the same
    // module's duplicate — this is the cheap first check inside
    // metadataDescriptionsAgree).
    {
        auto ca = seedCarried("LibA", "%0 > 0");
        auto cb = seedCarried("LibB", "%0 > 0");
        BEZEL_EXPECT_FALSE(metadataDescriptionsAgree(ca, cb));
    }
    // (4) Adding a member on one side (both carried-true → both carried,
    // but structural differs): full-record branch fires, returns false.
    {
        auto ca = seedCarried("Lib", "%0 > 0");
        auto cb = ca;
        MetaMember extra;
        extra.kind = MetaMemberKind::Field;
        extra.name = "Extra";
        extra.visibility = MetaVisibility::Public;
        extra.type = prim(PrimitiveKind::Int);
        cb.types[0].members.push_back(extra);
        BEZEL_EXPECT_FALSE(metadataDescriptionsAgree(ca, cb));
    }
}

} // namespace bezel::module