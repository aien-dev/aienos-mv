fn main() {
    let code = aienos_run::app::main(std::env::args().skip(1).collect());
    std::process::exit(code);
}
