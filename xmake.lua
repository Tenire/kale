add_rules("mode.debug", "mode.release")

set_project("kale")
set_version("0.1.0", {build = "%Y%m%d%H%M"})
set_languages("c++20")

-- Package repositories (remote GitHub repository by default; local-repo retained for local debugging)
add_repositories("t-xrepo https://github.com/Tenire/xrepo")
-- add_repositories("local-repo .")

package("workflow")
    set_base("workflow-ws")
package_end()
package("openssl")
    set_base("openssl3")
package_end()

-- Package dependencies (versions determined by package defaults)
add_requires("openssl", {system = false})
add_requires("zlib", {configs = {shared = false}})
add_requires("zstd", {system = false})
add_requires("workflow", {configs = {shared = false}})
add_requires("coke v0.7.0", {configs = {shared = false}})

-- Subdirectories
includes("src")
includes("tests")
includes("examples")
