set_project("ModlistPacker")
set_version("0.1.0")

add_rules("mode.debug", "mode.release")
set_languages("c++20")

target("modlist-packer")
    set_kind("binary")
    add_files("src/main.cpp")
    if is_plat("windows") then
        add_cxxflags("/W4", "/permissive-", {tools = "cl"})
        add_syslinks("user32", "gdi32", "comdlg32", "shell32", "ole32", "comctl32", "bcrypt")
        add_ldflags("/SUBSYSTEM:WINDOWS", {tools = "link"})
        set_runtimes("MT")
    end
target_end()
