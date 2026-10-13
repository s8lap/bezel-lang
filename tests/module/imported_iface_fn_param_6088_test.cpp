// fix #6088 / spec §11.2 (v0.338): a member's own type parameters correspond
// by position, not by name, in a function-typed PARAMETER and an inline-array
// generic argument (`(Box<T>) -> int`, `Box<[3]T>`), and a class-level
// substitution applies through a function-typed parameter or return. The same
// obligations hold for an interface IMPORTED from another module, including a
// generic one whose method's own parameter is spelled like the importing class's
// parameter (#6110).
//
// Every row is two-module (real Driver::compileObject + compileFile with a
// Dependency). Run rows assert stdout; reject rows assert the §11.2 diagnostic.

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <string>
#include <vector>

#include <fcntl.h>
#include <sys/wait.h>
#include <unistd.h>

#include "driver/Driver.hpp"
#include "support/driver_run_support.hpp"
#include "support/unique_stem.hpp"
#include "test_harness.hpp"

namespace bezel::module {
namespace {

namespace fs = std::filesystem;

struct Tmp6088 {
    std::string path;
    explicit Tmp6088(std::string p) : path(std::move(p)) {}
    ~Tmp6088() { std::remove(path.c_str()); }
};

void write6088(const std::string& path, const std::string& src) {
    std::ofstream out(path);
    out << src;
}

struct Result6088 {
    bool libBuilt = false;
    bool appCompiled = false;
    std::string err;
    std::string out;
    int code = -1;
};

Result6088 build6088(const std::string& libSrc, const std::string& appSrc) {
    auto dir = fs::temp_directory_path();
    std::string id = unique_stem("bz6088_");
    Tmp6088 libF((dir / (id + "_lib.bzl")).string());
    Tmp6088 libO((dir / (id + "_lib.o")).string());
    Tmp6088 appF((dir / (id + "_app.bzl")).string());
    Tmp6088 exeF((dir / id).string());
    write6088(libF.path, libSrc);
    write6088(appF.path, appSrc);
    Result6088 r;
    bezel::Driver driver;
    r.libBuilt = driver.compileObject(libF.path, libO.path).success &&
                 std::ifstream(libO.path).good();
    if (!r.libBuilt) return r;
    std::vector<bezel::Dependency> deps;
    bezel::Dependency d;
    d.metadataPath = libO.path;
    d.objectPath = libO.path;
    deps.push_back(d);

    std::string errPath = (dir / (id + "_err.txt")).string();
    std::fflush(stderr);
    std::cerr.flush();
    int saved = ::dup(STDERR_FILENO);
    int fd = ::open(errPath.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (fd >= 0) ::dup2(fd, STDERR_FILENO);
    r.appCompiled =
        bezel::test::compileFileForTest(driver, appF.path, exeF.path, deps)
            .success;
    std::fflush(stderr);
    std::cerr.flush();
    if (fd >= 0) {
        ::close(fd);
        ::dup2(saved, STDERR_FILENO);
    }
    ::close(saved);
    {
        std::ifstream in(errPath);
        r.err.assign((std::istreambuf_iterator<char>(in)),
                     std::istreambuf_iterator<char>());
        std::remove(errPath.c_str());
    }
    if (!r.appCompiled) return r;
    std::string cmd = exeF.path + " 2>&1";
    FILE* p = popen(cmd.c_str(), "r");
    if (!p) return r;
    char buf[256];
    while (std::size_t n = std::fread(buf, 1, sizeof(buf), p))
        r.out.append(buf, n);
    int status = pclose(p);
    r.code = (status != -1 && WIFEXITED(status)) ? WEXITSTATUS(status) : -1;
    return r;
}

const char* kLibPre = "module LibA; "
    "public class Box<T> public constructor() end constructor; end Box; ";

std::string app(const std::string& impl) {
    return "module AppB; import LibA; "
           "public class K: I public constructor() end constructor; " + impl +
           " end K; "
           "public class C public static method Main(): int "
           "  var s: I := new K(); Console.WriteLine(\"ok\"); return 0; "
           "end Main; end C;";
}

bool hasMismatch(const Result6088& r) {
    return r.err.find("does not match interface `I`'s") != std::string::npos &&
           r.err.find("(\xC2\xA7""11.2)") != std::string::npos;
}

void accept(const std::string& iface, const std::string& impl) {
    auto r = build6088(std::string(kLibPre) + "public interface I " + iface +
                       " end I;", app(impl));
    BEZEL_EXPECT_TRUE(r.libBuilt);
    BEZEL_EXPECT_TRUE(r.appCompiled);
    BEZEL_EXPECT_EQ(r.out, std::string("ok\n"));
}

void reject(const std::string& iface, const std::string& impl) {
    auto r = build6088(std::string(kLibPre) + "public interface I " + iface +
                       " end I;", app(impl));
    BEZEL_EXPECT_TRUE(r.libBuilt);
    BEZEL_EXPECT_TRUE(!r.appCompiled);
    BEZEL_EXPECT_TRUE(hasMismatch(r));
}

// #6110: a generic imported interface `I<S>` whose method's own type parameter
// `T` is spelled like the class parameter `T` of `K<T>: I<T>` in the importing
// module. The substituted-in class `T` is not the method's own `T`.
std::string appGeneric(const std::string& impl) {
    return "module AppB; import LibA; "
           "public class K<T>: I<T> public constructor() end constructor; " + impl +
           " end K; "
           "public class C public static method Main(): int "
           "  var s: I<int> := new K<int>(); Console.WriteLine(\"ok\"); return 0; "
           "end Main; end C;";
}

const char* kLibPair = "module LibA; "
    "public class Box<T> public constructor() end constructor; end Box; "
    "public class Pair<K, V> public constructor() end constructor; end Pair; ";

void acceptGeneric(const std::string& iface, const std::string& impl) {
    auto r = build6088(std::string(kLibPair) + "public interface I<S> " + iface +
                       " end I;", appGeneric(impl));
    BEZEL_EXPECT_TRUE(r.libBuilt);
    BEZEL_EXPECT_TRUE(r.appCompiled);
    BEZEL_EXPECT_EQ(r.out, std::string("ok\n"));
}

void rejectGeneric(const std::string& iface, const std::string& impl) {
    auto r = build6088(std::string(kLibPair) + "public interface I<S> " + iface +
                       " end I;", appGeneric(impl));
    BEZEL_EXPECT_TRUE(r.libBuilt);
    BEZEL_EXPECT_TRUE(!r.appCompiled);
    BEZEL_EXPECT_TRUE(hasMismatch(r));
}

}  // namespace

BEZEL_TEST(imported_iface_fn_param_6088, class_param_named_like_own_inline_array_runs) {
    acceptGeneric("method P<T>(f: Pair<[3]S, [2]T>): int;",
                  "public method P<U>(f: Pair<[3]T, [2]U>): int return 1; end P;");
}
BEZEL_TEST(imported_iface_fn_param_6088, class_param_named_like_own_plain_pair_runs) {
    acceptGeneric("method P<T>(f: Pair<S, T>): int;",
                  "public method P<U>(f: Pair<T, U>): int return 1; end P;");
}
BEZEL_TEST(imported_iface_fn_param_6088, own_param_in_class_slot_rejected) {
    rejectGeneric("method P<T>(f: Pair<[3]S, [2]T>): int;",
                  "public method P<U>(f: Pair<[3]U, [2]T>): int return 1; end P;");
}

BEZEL_TEST(imported_iface_fn_param_6088, renamed_function_param_runs) {
    accept("method P<T,U>(f: (Box<T>) -> int): int;",
           "public method P<A,B>(f: (Box<A>) -> int): int return 1; end P;");
}
BEZEL_TEST(imported_iface_fn_param_6088, renamed_inline_array_arg_runs) {
    accept("method P<T,U>(a: Box<[3]T>): int;",
           "public method P<A,B>(a: Box<[3]A>): int return 1; end P;");
}
BEZEL_TEST(imported_iface_fn_param_6088, renamed_function_return_runs) {
    accept("method P<T,U>(): (T) -> U;",
           "public method P<A,B>(): (A) -> B throw new Exception(\"x\"); end P;");
}
BEZEL_TEST(imported_iface_fn_param_6088, function_param_swap_rejected) {
    reject("method P<T,U>(f: (Box<T>) -> int): int;",
           "public method P<A,B>(f: (Box<B>) -> int): int return 1; end P;");
}
BEZEL_TEST(imported_iface_fn_param_6088, inline_array_arg_swap_rejected) {
    reject("method P<T,U>(a: Box<[3]T>): int;",
           "public method P<A,B>(a: Box<[3]B>): int return 1; end P;");
}
BEZEL_TEST(imported_iface_fn_param_6088, function_return_swap_rejected) {
    reject("method P<T,U>(): (T) -> U;",
           "public method P<A,B>(): (B) -> A throw new Exception(\"x\"); end P;");
}

}  // namespace bezel::module
