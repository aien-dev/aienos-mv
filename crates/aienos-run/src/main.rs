fn main() {
    let code = aienos_run::app::main(std::env::args().skip(1).collect());
    aienos_run::console::hold_window_on_exit();
    std::process::exit(code);
}
