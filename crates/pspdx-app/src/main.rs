#![no_std]
#![no_main]

psp::module!("pspdx", 1, 0);

const VERSION: &str = env!("CARGO_PKG_VERSION");

fn psp_main() {
    // Without this the only way out is the power switch.
    psp::enable_home_button();

    psp::dprintln!("PSPDX {}", VERSION);
    psp::dprintln!("PSP Download Index");
    // Note: dprintln!() with no arguments does not compile — the zero-argument
    // arm calls a `dprint` function that does not exist. Pass "" instead.
    psp::dprintln!("");
    psp::dprintln!("Nothing to install yet.");
    psp::dprintln!("Press HOME to exit.");
}
