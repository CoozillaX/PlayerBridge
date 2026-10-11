add_rules("mode.debug", "mode.release")

add_repositories("levimc-repo https://github.com/LiteLDev/xmake-repo.git")

-- This plugin supports the Windows dedicated server only.
add_requires("levilamina 26.51.6", {configs = {target_type = "server"}})

add_requires("levibuildscript")
add_requires("nlohmann_json v3.12.0")

if not has_config("vs_runtime") then
    set_runtimes("MD")
end

target("NeteasePlayerBridge")
    add_rules("@levibuildscript/linkrule")
    add_rules("@levibuildscript/modpacker")
    if is_plat("windows") then
        add_defines("NOMINMAX", "UNICODE")
        set_exceptions("none") -- To avoid conflicts with /EHa.
        add_cxflags( "/EHa", "/utf-8", "/W4", "/w44265", "/w44289", "/w44296", "/w45263", "/w44738", "/w45204")
        add_cxflags(
            "/EHs",
            "-Wno-microsoft-cast",
            "-Wno-invalid-offsetof",
            "-Wno-c++2b-extensions",
            "-Wno-microsoft-include",
            "-Wno-overloaded-virtual",
            "-Wno-ignored-qualifiers",
            "-Wno-missing-field-initializers",
            "-Wno-potentially-evaluated-expression",
            "-Wno-pragma-system-header-outside-header",
            {tools = {"clang_cl"}}
        )
        set_toolchains("clang-cl")
    end

    add_packages("levilamina")
    add_syslinks("winhttp", "bcrypt")
    set_kind("shared")
    set_languages("c++20")
    set_symbols("debug")
    add_headerfiles("src/**.h")
    add_files("src/**.cpp")
    add_includedirs("src")

target("core-tests")
    set_default(false)
    set_kind("binary")
    set_languages("c++20")
    set_toolchains("clang-cl")
    add_cxflags("/utf-8", "/EHsc")
    add_defines("NOMINMAX")
    add_includedirs("src")
    add_packages("nlohmann_json")
    add_syslinks("winhttp", "bcrypt")
    add_files("tests/CoreTests.cpp", "src/profiles/Identity.cpp", "src/profiles/Files.cpp",
              "src/profiles/NbtSanitizer.cpp", "src/profiles/ApiClient.cpp", "src/profiles/ProfileRegistry.cpp")

target("NeteasePlayerBridgeFixture")
    set_default(false)
    set_kind("shared")
    set_languages("c++20")
    set_symbols("debug")
    set_toolchains("clang-cl")
    add_rules("@levibuildscript/linkrule", "@levibuildscript/modpacker")
    add_defines("NOMINMAX", "UNICODE")
    add_cxflags("/utf-8", "/EHsc", "-Wno-invalid-" .. "offsetof")
    add_packages("levilamina")
    add_syslinks("bcrypt")
    add_includedirs("src")
    add_files("tests/StorageFixture.cpp", "tests/RegistryStoreTests.cpp", "src/profiles/RegistryStore.cpp", "src/mod/MemoryOperators.cpp", "src/profiles/Storage.cpp", "src/profiles/ProfileRegistry.cpp",
              "src/profiles/Identity.cpp", "src/profiles/Files.cpp", "src/profiles/NbtSanitizer.cpp")
