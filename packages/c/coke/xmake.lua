package("coke")
    set_homepage("https://github.com/kedixa/coke")
    set_description("C++20 Coroutine Framework based on Sogou Workflow")
    set_license("Apache-2.0")

    add_urls("https://github.com/kedixa/coke/archive/refs/tags/$(version).tar.gz",
             "https://github.com/kedixa/coke.git")

    add_versions("v0.7.0", "9d7ccf0a67b837e235369a3dfdb4feba7ea31909f7e0cb0a7cdfb36c8f996b58")

    add_deps("workflow")
    add_deps("openssl3")

    if is_plat("linux") then
        add_syslinks("pthread", "dl")
    end

    on_install("linux", "macosx", function (package)
        os.cp(path.join(package:scriptdir(), "port", "xmake.lua"), "xmake.lua")
        local configs = {}
        if package:config("shared") then
            configs.kind = "shared"
        end
        import("package.tools.xmake").install(package, configs)
    end)

    on_test(function (package)
        assert(package:check_cxxsnippets({test = [[
            #include <coke/coke.h>
            coke::Task<> example() {
                co_return;
            }
            void test() {
                coke::sync_wait(example());
            }
        ]]}, {configs = {languages = "c++20"}}))
    end)
package_end()
