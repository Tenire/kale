target("examples")
    set_kind("phony")
    set_default(false)
    add_deps(
        "example_hello",
        "example_routing",
        "example_middleware",
        "example_json_api",
        "example_typed_api",
        "example_forms",
        "example_file_service",
        "example_uploads",
        "example_resumable_upload",
        "example_event_stream",
        "example_sessions",
        "example_jwt_auth",
        "example_secure_forms",
        "example_http_client",
        "example_reverse_proxy",
        "example_operations",
        "example_prometheus",
        "example_openapi",
        "example_cookie_auth",
        "example_chunked_stream",
        "example_nested_blueprints",
        "example_error_handling",
        "example_access_log"
    )

local example_targets = {
    -- Core module targets
    {name = "hello",            deps = {"kale_core"}},
    {name = "routing",          deps = {"kale_core"}},
    {name = "middleware",       deps = {"kale_core"}},

    -- HTTP module targets
    {name = "json_api",         deps = {"kale_http"}},
    {name = "typed_api",        deps = {"kale_http"}},
    {name = "forms",            deps = {"kale_http"}},
    {name = "file_service",     deps = {"kale_http"}},
    {name = "uploads",          deps = {"kale_http"}},
    {name = "resumable_upload", deps = {"kale_http", "kale_security"}},
    {name = "event_stream",     deps = {"kale_http"}},

    -- Security module targets
    {name = "sessions",         deps = {"kale_security"}},
    {name = "jwt_auth",         deps = {"kale_security"}},
    {name = "secure_forms",     deps = {"kale_security"}},

    -- Client module targets
    {name = "http_client",      deps = {"kale_client"}},
    {name = "reverse_proxy",    deps = {"kale_client"}},

    -- Operations & compression targets
    {name = "operations",        deps = {"kale_http"}},
    {name = "access_log",        deps = {"kale_http", "kale_core"}},

    -- Additional framework capability examples
    {name = "prometheus",        deps = {"kale_http"}},
    {name = "openapi",           deps = {"kale_http"}},
    {name = "cookie_auth",       deps = {"kale_security", "kale_core"}},
    {name = "chunked_stream",    deps = {"kale_core"}},
    {name = "nested_blueprints", deps = {"kale_core"}},
    {name = "error_handling",    deps = {"kale_http", "kale_core"}},
}

for _, target_info in ipairs(example_targets) do
    target("example_" .. target_info.name)
        set_kind("binary")
        set_default(false)
        set_warnings("all", "extra")
        set_rundir("$(projectdir)")
        add_includedirs(".")
        for _, dep in ipairs(target_info.deps) do
            add_deps(dep)
        end
        add_files(target_info.name .. ".cpp")
end
