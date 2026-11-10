add_rules("mode.debug", "mode.release")

add_requires("workflow")
add_requires("openssl3")

target("coke")
    set_kind("$(kind)")
    set_languages("c++20")
    add_files("src/**.cpp")
    add_headerfiles("include/(**)")
    add_includedirs("include", {public = true})
    add_packages("workflow", "openssl3")
    if is_plat("linux") then
        add_syslinks("pthread", "dl")
    end
