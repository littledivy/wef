// Copyright 2025 Divy Srivastava. All rights reserved. MIT license.

use std::env;
use std::path::PathBuf;

fn main() {
    let header = "include/wef.h";
    println!("cargo:rerun-if-changed={}", header);

    let bindings = bindgen::Builder::default()
        .header(header)
        .parse_callbacks(Box::new(bindgen::CargoCallbacks::new()))
        .allowlist_type("wef_.*")
        .allowlist_var("WEF_.*")
        .generate()
        .expect("Unable to generate bindings");

    let out_path = PathBuf::from(env::var("OUT_DIR").unwrap());
    bindings
        .write_to_file(out_path.join("bindings.rs"))
        .expect("Couldn't write bindings!");
}
