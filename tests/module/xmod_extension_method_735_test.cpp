// Cross-module EXTENSION method dispatch (#735 part B1; §26.5 / §2.5 / §2.4,
// spec v0.226).
//
// §26.5 step 2 is normative: "If the receiver's type declares no instance
// member of that name, the compiler searches the current module AND ALL
// IMPORTED MODULES for extension blocks on x's type, base types, or implemented
// interfaces". The implementation never met that rule across an artifact
// boundary: `ModuleMetadataBuilder::buildModuleMetadata` skipped every non-
// TypeSymbol module child (extension blocks are `ExtensionSymbol`s), the schema
// had no carrier for them, and `ModuleLoader` had no reconstruction — so an
// imported `public extension Widget ... Double()` was invisible to
// ExprTC::lookupExtensionMember. The #735 repro (`w.Double()` on an imported
// Widget) originally MISCOMPILED to a garbage exit; by the time this test was
// written the manifestation had degraded to a (sound but wrong) reject,
// "no member `Double` on type `Widget`". Either way the call never dispatched.
//
// WHY THESE FAIL WITHOUT THE FIX: with no extension carriage the importer emits
// a member-not-found diagnostic, so `compileFile` fails and importRun735ext
// returns -1 — never the asserted VALUE. They pass only once (a) the builder
// exports the block, (b) the loader reconstructs it as an ExtensionSymbol with
// a live ast::ExtensionDecl, (c) lookupExtensionMember searches the imported
// set, and (d) codegen declares the imported `bz$ext$<T>$<M>` with the §26.1
// receiver ABI (by VALUE for a primitive/enum head, by pointer for a
// class/interface/record head) and calls it.
//
// The value assertions are the point: a wrong-receiver read is a SILENT
// wrong-value class, so each case pins an arithmetic result that can only be
// produced by reading the right receiver (21*2, 40+2, Color.Green's arm, 7*3).

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <sys/wait.h>

#include "driver/Driver.hpp"
#include "module/ModuleMetadata.hpp"
#include "test_harness.hpp"
#include "support/unique_stem.hpp"


namespace {

struct TempFile735x {
    std::string path;
    explicit TempFile735x(std::string p) : path(std::move(p)) {}
    ~TempFile735x() { std::remove(path.c_str()); }
};

void write735x(const std::string& path, const std::string& src) {
    std::ofstream out(path);
    out << src;
}

bool compileObj735x(const std::string& src, const std::string& obj) {
    bezel::Driver driver;
    return driver.compileObject(src, obj).success;
}

// Compile the importer against `aObj`, run it, and return its exit code.
// -1 = the importer failed to compile/link or the program did not exit
// normally (which is exactly what happens without the fix).
int importRun735x(const std::string& bSrc, const std::string& bExe,
                  const std::string& aObj) {
    bezel::Driver driver;
    std::vector<bezel::Dependency> deps;
    bezel::Dependency d;
    d.metadataPath = aObj;
    d.objectPath = aObj;
    deps.push_back(d);
    if (!driver.compileFile(bSrc, bExe, deps).success) return -1;
    int status = std::system(bExe.c_str());
    return (status != -1 && WIFEXITED(status)) ? WEXITSTATUS(status) : -1;
}

// Does the importer COMPILE at all? Used by the §13.2 negative case, where the
// contract is a clean reject (never a wrong value).
bool importCompiles735x(const std::string& bSrc, const std::string& bExe,
                        const std::string& aObj) {
    bezel::Driver driver;
    std::vector<bezel::Dependency> deps;
    bezel::Dependency d;
    d.metadataPath = aObj;
    d.objectPath = aObj;
    deps.push_back(d);
    return driver.compileFile(bSrc, bExe, deps).success;
}

}  // namespace

// #735 B1, the issue's own repro verbatim: a `public extension Widget` in a
// library module, called from an importer. `Double()` reads `this._val` (21)
// and returns 42. Before the fix the importer could not resolve `Double` at
// all, so this returned -1.  §26.5 step 2 / §26.1.
BEZEL_TEST(xmod_extension_735, imported_extension_method_class_receiver_26_5) {
    namespace fs = std::filesystem;
    auto dir = fs::temp_directory_path();
    std::string stem = unique_stem("bz735x_cls_");
    TempFile735x aSrc((dir / (stem + "_a.bzl")).string());
    TempFile735x aObj((dir / (stem + "_a.o")).string());

    write735x(aSrc.path,
        "module ExtLib735; "
        "public class Widget "
        "  public _val: int; "
        "  public constructor(v: int) _val := v; end constructor; "
        "  public method GetVal(): int => _val; "
        "end Widget; "
        "public extension Widget "
        "  public method Double(): int => this._val * 2; "
        "end;");

    BEZEL_EXPECT_TRUE(compileObj735x(aSrc.path, aObj.path));
    if (!std::ifstream(aObj.path).good()) return;

    TempFile735x bSrc((dir / (stem + "_b.bzl")).string());
    TempFile735x bExe((dir / (stem + "_b")).string());
    write735x(bSrc.path,
        "module App; "
        "import ExtLib735; "
        "class C public static method Main(): int "
        "  var w := new Widget(21); "
        "  var d := w.Double(); "
        "  w.Dispose(); "
        "  return d; "          // 21 * 2 = 42
        "end Main; end C;");

    BEZEL_EXPECT_EQ(importRun735x(bSrc.path, bExe.path, aObj.path), 42);
}

// #735 B1 / §26.1 + §26.2: the RECEIVER ABI across the boundary, for every
// receiver kind §26.2 admits. A class receiver is the object pointer, a record
// receiver a pointer to a spilled value, an enum receiver the underlying
// integer BY VALUE, a primitive receiver the value BY VALUE. Declaring any of
// them as a plain `ptr` on the importer side (what declareImportedFunctions /
// methodFnType did for every parentless method) passes the wrong thing where
// `bz$ext$int$Triple(i32)` expects an i32 — the silent wrong-value shape #735
// was originally filed for. Each summand is distinct so a single wrong receiver
// changes the total.
BEZEL_TEST(xmod_extension_735, imported_extension_receiver_abi_all_kinds_26_2) {
    namespace fs = std::filesystem;
    auto dir = fs::temp_directory_path();
    std::string stem = unique_stem("bz735x_abi_");
    TempFile735x aSrc((dir / (stem + "_a.bzl")).string());
    TempFile735x aObj((dir / (stem + "_a.o")).string());

    write735x(aSrc.path,
        "module ExtLib735Abi; "
        "public class Widget "
        "  public _val: int; "
        "  public constructor(v: int) _val := v; end constructor; "
        "end Widget; "
        "public record Point(x: int, y: int); "
        "public enum Color Red, Green, Blue; end Color; "
        "public extension Widget "
        "  public method Double(): int => this._val * 2; "
        "  public method AddN(n: int): int => this._val + n; "
        "end; "
        "public extension Point "
        "  public method Sum(): int => this.x + this.y; "
        "end; "
        "public extension Color "
        "  public method Code(): int => (case this of "
        "    Color.Red => 10, Color.Green => 20, Color.Blue => 30, _ => -1); "
        "end; "
        "public extension int "
        "  public method Triple(): int => this * 3; "
        "end;");

    BEZEL_EXPECT_TRUE(compileObj735x(aSrc.path, aObj.path));
    if (!std::ifstream(aObj.path).good()) return;

    TempFile735x bSrc((dir / (stem + "_b.bzl")).string());
    TempFile735x bExe((dir / (stem + "_b")).string());
    write735x(bSrc.path,
        "module App; "
        "import ExtLib735Abi; "
        "class C public static method Main(): int "
        "  var w := new Widget(21); "
        "  var r := w.Double(); "          // class receiver    -> 42
        "  r := r + w.AddN(8); "           // + extension arg   -> 29
        "  w.Dispose(); "
        "  var pt := new Point(40, 2); "
        "  r := r + pt.Sum(); "            // record receiver   -> 42
        "  var c := Color.Green; "
        "  r := r + c.Code(); "            // enum receiver     -> 20
        "  var x := 7; "
        "  r := r + x.Triple(); "          // primitive receiver-> 21
        "  return r; "                     // 42+29+42+20+21 = 154
        "end Main; end C;");

    BEZEL_EXPECT_EQ(importRun735x(bSrc.path, bExe.path, aObj.path), 154);
}

// §26.5 step 2 explicitly covers extensions on "base types, or implemented
// interfaces": an imported `public extension IShape` must be found for a
// receiver of an imported class that implements IShape, and the extension body's
// `this.Area()` must dispatch through that receiver. 5*5*2 = 50.
BEZEL_TEST(xmod_extension_735, imported_extension_on_interface_receiver_26_5) {
    namespace fs = std::filesystem;
    auto dir = fs::temp_directory_path();
    std::string stem = unique_stem("bz735x_ifc_");
    TempFile735x aSrc((dir / (stem + "_a.bzl")).string());
    TempFile735x aObj((dir / (stem + "_a.o")).string());

    write735x(aSrc.path,
        "module ExtLib735Ifc; "
        "public interface IShape "
        "  method Area(): int; "
        "end IShape; "
        "public class Sq: IShape "
        "  public _s: int; "
        "  public constructor(s: int) _s := s; end constructor; "
        "  public method Area(): int => _s * _s; "
        "end Sq; "
        "public extension IShape "
        "  public method Twice(): int => this.Area() * 2; "
        "end;");

    BEZEL_EXPECT_TRUE(compileObj735x(aSrc.path, aObj.path));
    if (!std::ifstream(aObj.path).good()) return;

    TempFile735x bSrc((dir / (stem + "_b.bzl")).string());
    TempFile735x bExe((dir / (stem + "_b")).string());
    write735x(bSrc.path,
        "module App; "
        "import ExtLib735Ifc; "
        "class C public static method Main(): int "
        "  var s := new Sq(5); "
        "  var r := s.Twice(); "
        "  s.Dispose(); "
        "  return r; "          // (5*5) * 2 = 50
        "end Main; end C;");

    BEZEL_EXPECT_EQ(importRun735x(bSrc.path, bExe.path, aObj.path), 50);
}

// §26.7: "Extensions may return `owned` values." The `owned` bit rides the
// metadata (MetaMember::returnOwned) and must survive reconstruction, or the
// importer's `using var c := w.Clone()` either fails to register the disposal
// (leak) or disposes a borrow (double-free). 41 + 1 = 42, with the clone
// disposed by the `using var` and the original by the explicit Dispose().
BEZEL_TEST(xmod_extension_735, imported_extension_owned_return_26_7) {
    namespace fs = std::filesystem;
    auto dir = fs::temp_directory_path();
    std::string stem = unique_stem("bz735x_own_");
    TempFile735x aSrc((dir / (stem + "_a.bzl")).string());
    TempFile735x aObj((dir / (stem + "_a.o")).string());

    write735x(aSrc.path,
        "module ExtLib735Own; "
        "public class Widget "
        "  public _val: int; "
        "  public constructor(v: int) _val := v; end constructor; "
        "  public method GetVal(): int => _val; "
        "end Widget; "
        "public extension Widget "
        "  public method Clone(): owned Widget => new Widget(this._val + 1); "
        "end;");

    BEZEL_EXPECT_TRUE(compileObj735x(aSrc.path, aObj.path));
    if (!std::ifstream(aObj.path).good()) return;

    TempFile735x bSrc((dir / (stem + "_b.bzl")).string());
    TempFile735x bExe((dir / (stem + "_b")).string());
    write735x(bSrc.path,
        "module App; "
        "import ExtLib735Own; "
        "class C public static method Main(): int "
        "  var w := new Widget(41); "
        "  using var c := w.Clone(); "
        "  var r := c.GetVal(); "
        "  w.Dispose(); "
        "  return r; "          // 41 + 1 = 42
        "end Main; end C;");

    BEZEL_EXPECT_EQ(importRun735x(bSrc.path, bExe.path, aObj.path), 42);
}

// §13.2 / §26.1: an extension block's default reach is `internal`, so it is NOT
// part of the exported surface. The importer must keep rejecting the call — the
// carriage added for #735 must not widen visibility. A clean reject, never a
// wrong value.
BEZEL_TEST(xmod_extension_735, internal_extension_is_not_exported_13_2) {
    namespace fs = std::filesystem;
    auto dir = fs::temp_directory_path();
    std::string stem = unique_stem("bz735x_int_");
    TempFile735x aSrc((dir / (stem + "_a.bzl")).string());
    TempFile735x aObj((dir / (stem + "_a.o")).string());

    write735x(aSrc.path,
        "module ExtLib735Int; "
        "public class Widget "
        "  public _val: int; "
        "  public constructor(v: int) _val := v; end constructor; "
        "end Widget; "
        "extension Widget "                 // no `public` → §26.1 internal
        "  public method Hidden(): int => this._val * 2; "
        "end;");

    BEZEL_EXPECT_TRUE(compileObj735x(aSrc.path, aObj.path));
    if (!std::ifstream(aObj.path).good()) return;

    TempFile735x bSrc((dir / (stem + "_b.bzl")).string());
    TempFile735x bExe((dir / (stem + "_b")).string());
    write735x(bSrc.path,
        "module App; "
        "import ExtLib735Int; "
        "class C public static method Main(): int "
        "  var w := new Widget(21); "
        "  var d := w.Hidden(); "
        "  w.Dispose(); "
        "  return d; "
        "end Main; end C;");

    BEZEL_EXPECT_FALSE(importCompiles735x(bSrc.path, bExe.path, aObj.path));
}

// §2.5 / §26 wire-format pin: MetaExtension survives a serialize→deserialize
// roundtrip, and it is part of the semantic hash (adding an extension member
// must mark dependents stale). Also pins the back-compat rule the carriage was
// designed around: metadata with NO exported extension serializes to exactly
// the bytes it did before the field existed, so a pre-#735 reader is unaffected.
BEZEL_TEST(xmod_extension_735, metadata_extension_roundtrip_and_hash_2_5) {
    using namespace bezel::module;

    ModuleMetadata md;
    md.moduleName = "ExtLib735";

    MetaExtension ext;
    ext.target.kind = TypeKind::Named;
    ext.target.namedName = "Widget";
    ext.visibility = MetaVisibility::Public;
    MetaMember m;
    m.kind = MetaMemberKind::Method;
    m.name = "Double";
    m.visibility = MetaVisibility::Public;
    m.type.kind = TypeKind::Primitive;
    m.type.primitive = PrimitiveKind::Int;
    ext.members.push_back(std::move(m));

    // Byte-identical to a pre-#735 blob while `extensions` is empty.
    const std::vector<std::uint8_t> without = serialize(md);
    const std::uint64_t hashWithout = semanticHash(md);

    md.extensions.push_back(std::move(ext));
    const std::vector<std::uint8_t> with = serialize(md);

    auto back = deserialize(with);
    BEZEL_EXPECT_TRUE(back.has_value());
    if (!back) return;
    BEZEL_EXPECT_EQ(back->extensions.size(), std::size_t{1});
    BEZEL_EXPECT_TRUE(*back == md);
    BEZEL_EXPECT_EQ(back->extensions[0].target.namedName, std::string("Widget"));
    BEZEL_EXPECT_EQ(back->extensions[0].members.size(), std::size_t{1});
    BEZEL_EXPECT_EQ(back->extensions[0].members[0].name, std::string("Double"));

    // The exported extension changes the public-interface hash (staleness).
    BEZEL_EXPECT_TRUE(semanticHash(md) != hashWithout);
    // And an extension-free module is unaffected by the new carrier.
    ModuleMetadata plain;
    plain.moduleName = "ExtLib735";
    BEZEL_EXPECT_TRUE(serialize(plain) == without);
    BEZEL_EXPECT_EQ(semanticHash(plain), hashWithout);
}

// ===========================================================================
// fix #3828 (§26.1 / §26.5 step 2 / §2.8.2 clause 1, v0.287) — BOTH loader
// publishes of `returnTypes_` / `fieldTypes_` must run, and this row fails if
// EITHER is removed.
//
// `ModuleLoader::loadMetadataPass2` fills two side tables while it walks the
// metadata (`loader.returnTypes_` per imported method, `loader.fieldTypes_`
// per imported field/property) and publishes them into the SemaContext with
// `setImportedReturnType` / `setImportedMemberType`. The publish used to sit
// at the END of that function. #3748 (v0.286) MOVED it to just before pass 3
// — pass 3's `parseAndAttachFieldInit` re-parses an imported generic's field
// initializer against a throwaway SemaContext and is the first consumer that
// reads these tables — and deleted the trailing copy.
//
// That move silently dropped everything pass FOUR adds. Pass 4 is where an
// imported EXTENSION block's members are reconstructed, and it is the writer
// of `loader.returnTypes_[ms]` for every extension METHOD and
// `loader.fieldTypes_[ps]` for every extension PROPERTY. With no published
// `importedReturnType`, the §26.5-resolved call `x.Triple()` carried no
// exprType, so codegen had nothing to lower and degraded the enclosing slot
// to a zero fill — the `#2434` / `#2491` / `#2961` "failed to lower to a
// value" warning family, exit status 0, WRONG VALUE. #3828 restores the
// trailing publish while keeping the #3748 early one.
//
// The row pins both halves with ONE observable so neither publish can be
// deleted as "redundant":
//
//   * the #3828 half — `x.Triple()` on the dependency's `public extension
//     int` — contributes 7 * 3 = 21. Without the LATE publish this degrades
//     to 0 and the total reads 6.
//   * the #3748 half — `G735P<int>`'s `b: string := H735P.Str() +
//     H735P.Str()` (two NON-constant string operands on an imported generic)
//     — contributes `g.b.Length` = 6 ("s42s42", a code-point count, stdlib
//     §4.6). Without the EARLY publish this is the #3748 malformed-IR shape:
//     codegen emits `add ptr` and the LLVM verifier aborts, so the importer
//     never links and `importRun735x` returns -1.
//
// 21 + 6 = 27, and each failure mode has a distinct wrong answer (6 / -1).
// ===========================================================================
BEZEL_TEST(xmod_extension_735,
           imported_extension_method_return_type_published_after_pass4_26_1) {
    namespace fs = std::filesystem;
    auto dir = fs::temp_directory_path();
    std::string stem = unique_stem("bz735x_pub_");
    TempFile735x aSrc((dir / (stem + "_a.bzl")).string());
    TempFile735x aObj((dir / (stem + "_a.o")).string());

    // One dependency carrying BOTH consumers: an imported generic whose field
    // initializer pass 3 re-parses (needs the EARLY publish) and an
    // extension-on-`int` block pass 4 builds (needs the LATE publish).
    write735x(aSrc.path,
        "module ExtLib735Pub; "
        "public class H735P "
        "  public static method Str(): string => \"s42\"; "
        "end H735P; "
        "public class G735P<T> "
        "  public v: T; "
        "  public b: string := H735P.Str() + H735P.Str(); "
        "  public constructor(v0: T) v := v0; end constructor; "
        "end G735P; "
        "public extension int "
        "  public method Triple(): int => this * 3; "
        "end;");

    BEZEL_EXPECT_TRUE(compileObj735x(aSrc.path, aObj.path));
    if (!std::ifstream(aObj.path).good()) return;

    TempFile735x bSrc((dir / (stem + "_b.bzl")).string());
    TempFile735x bExe((dir / (stem + "_b")).string());
    write735x(bSrc.path,
        "module App; "
        "import ExtLib735Pub; "
        "class C public static method Main(): int "
        "  var g := new G735P<int>(9); "
        "  var n := g.b.Length; "        // 6 — the #3748 (early publish) half
        "  var x: int := 7; "
        "  var t := x.Triple(); "        // 21 — the #3828 (late publish) half
        "  return t + n; "               // 21 + 6 = 27
        "end Main; end C;");

    BEZEL_EXPECT_EQ(importRun735x(bSrc.path, bExe.path, aObj.path), 27);
}
