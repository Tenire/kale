target("kale_core")
    set_kind("static")
    set_warnings("all", "extra")
    add_configfiles("$(projectdir)/src/version.h.in", {filename = "kale/version.h"})
    add_headerfiles("$(projectdir)/include/kale/**.h", {rootdir = "$(projectdir)/include"})
    add_headerfiles("$(builddir)/kale/version.h", {rootdir = "$(builddir)"})
    add_includedirs("$(projectdir)/include",
                    "$(builddir)",
                    {public = true})
    add_includedirs("$(projectdir)/src")
    add_files("core/*.cpp")
    add_packages("workflow", "coke", "zlib", "zstd", {public = true})
    if is_plat("linux") then
        add_syslinks("pthread", "dl", {public = true})
    end

target("kale_http")
    set_kind("static")
    set_warnings("all", "extra")
    add_deps("kale_core", {public = true})
    add_includedirs("$(projectdir)/include",
                    "$(builddir)",
                    {public = true})
    add_includedirs("$(projectdir)/src")
    add_files("http/*.cpp", "middleware/*.cpp")

target("kale_security")
    set_kind("static")
    set_warnings("all", "extra")
    add_deps("kale_core", "kale_http", {public = true})
    add_packages("openssl", {public = true})
    add_includedirs("$(projectdir)/include",
                    "$(builddir)",
                    {public = true})
    add_includedirs("$(projectdir)/src")
    add_files("security/*.cpp")

target("kale_client")
    set_kind("static")
    set_warnings("all", "extra")
    add_deps("kale_core", "kale_http", {public = true})
    add_includedirs("$(projectdir)/include",
                    "$(builddir)",
                    {public = true})
    add_includedirs("$(projectdir)/src")
    add_files("client/*.cpp")

target("kale")
    set_kind("phony")
    set_warnings("all", "extra")
    add_deps("kale_client", "kale_security", "kale_http", "kale_core", {public = true})
    on_install(function (target)
        import("target.action.install", {alias = "install_target"})
        for _, dep in ipairs(target:orderdeps()) do
            install_target(dep)
        end
    end)
    on_uninstall(function (target)
        import("target.action.uninstall", {alias = "uninstall_target"})
        for _, dep in ipairs(target:orderdeps()) do
            uninstall_target(dep)
        end
    end)
