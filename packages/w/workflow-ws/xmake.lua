package("workflow-ws")
    set_homepage("https://github.com/Tenire/workflow-ws")
    set_description("Fork of sogou/workflow integrating WebSocket and Channel extensions from gnblao/workflow.")
    set_license("Apache-2.0")

    add_urls("https://github.com/Tenire/workflow-ws.git")
    add_versions("v1.1.1+ws", "a8bdec3506ed32dd5605fee26fe17fd0f7309bcb")
    add_versions("v1.1.0+ws", "db6972de1e26aef84a7525d3c53f2d42f79c5538")

    add_deps("openssl3")

    if is_plat("linux") then
        add_syslinks("pthread", "dl")
    end

    on_install("linux", "macosx", function (package)
        local configs = {}
        local content = io.readfile("xmake.lua")
        if content then
            content = content:gsub('add_requires%("openssl"%)', 'add_requires("openssl3")')
            content = content:gsub('add_packages%("openssl"%)', 'add_packages("openssl3")')
            content = content:gsub('includes%s*%b()', 'includes("src")')
            content = content:gsub('set_config%("buildir",.-%)\n', '')
            io.writefile("xmake.lua", content)
        end
        if package:config("shared") then
            configs.kind = "shared"
        end
        import("package.tools.xmake").install(package, configs)
        if os.isdir("_include/workflow") then
            os.cp("_include/workflow", package:installdir("include"))
        end
    end)

    on_test(function (package)
        assert(package:check_cxxsnippets({test = [[
            #include <workflow/WFTaskFactory.h>
            #include <workflow/WFWebSocketServer.h>
            #include <workflow/StreamReply.h>
            void test() {
                WFWebSocketServer server;
                StreamReply *reply = nullptr;
                (void)reply;
            }
        ]]}, {configs = {languages = "c++11"}}))
    end)
package_end()
