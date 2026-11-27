fn main() {
    println!("cargo:rerun-if-changed=native");
    println!("cargo:rerun-if-changed=Captura.ico");
    cc::Build::new()
        .cpp(true)
        .flag("/std:c++17")
        .flag("/utf-8")
        .flag("/EHsc")
        .define("UNICODE", None)
        .define("_UNICODE", None)
        .define("NOMINMAX", None)
        .file("native/app.cpp")
        .compile("captura_ui");
    for lib in [
        "gdiplus", "gdi32", "user32", "comctl32", "comdlg32", "ole32", "shell32", "dwmapi", "uuid",
    ] {
        println!("cargo:rustc-link-lib={lib}");
    }
    winresource::WindowsResource::new()
        .set_icon("Captura.ico")
        .set_manifest_file("native/app.manifest")
        .append_rc_content(include_str!("native/dialogs.rc"))
        .set("ProductName", "Captura")
        .set("FileDescription", "Captura")
        .set("CompanyName", "Project Gloria")
        .compile()
        .expect("embed Windows resources");
}
