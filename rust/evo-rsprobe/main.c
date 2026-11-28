/* Copyright (c) 2026 CallMeLewis
 * The C main of evo-rsprobe (rust/evo-rsprobe/src/lib.rs), so that the build image's GCC 4.7 links the program
 * against the copy of the car's system, as every glibc Rust program is linked (docs/rust-port.md, step 2). */
int evo_rsprobe_main(void);

int main(void) {
    return evo_rsprobe_main();
}
