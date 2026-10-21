#include <cstring>

#include <iostream>
#include <print>

#include <mim/driver.h>
#include <mim/nest.h>

#include <mim/ast/parser.h>
#include <mim/phase/beta_red.h>
#include <mim/phase/eta_conv.h>

#include "divergence.h"

/// Usage: `divergence [--dump] [--raw] [--nest] [--jumps] <file.mim>`
/// - `--dump`:  also dump the World that gets analysed.
/// - `--raw`:   skip the BetaRed/EtaConv clean-up before the analysis.
///
int main(int argc, char** argv) {
    bool dump = false;
    bool raw  = false;

    const char* input = nullptr;

    for (int i = 1; i < argc; ++i)
        if (std::strcmp(argv[i], "--dump") == 0)
            dump = true;
        else if (std::strcmp(argv[i], "--raw") == 0)
            raw = true;
        else
            input = argv[i];

    if (!input) {
        std::println(std::cerr, "usage: {} [--dump] [--raw] <file.mim>", argv[0]);
        return EXIT_FAILURE;
    }

    try {
        auto driver = mim::Driver();
        auto& world = driver.world();
        auto ast    = mim::ast::AST(world);
        auto parser = mim::ast::Parser(ast);

        if (auto mod = parser.import_main(input, {})) {
            mod->compile(ast);
            if (!raw) {
                mim::Phase::run<mim::BetaRed>(world);
                mim::Phase::run<mim::EtaConv>(world);
            }
            if (dump) world.dump();

            auto analysis = mim::Divergence(world);
            analysis.run_to_fp();
            analysis.dump(std::cout);
        }
    } catch (const std::exception& e) {
        std::println(std::cerr, "{}", e.what());
        return EXIT_FAILURE;
    } catch (...) {
        std::println(std::cerr, "error: unknown exception");
        return EXIT_FAILURE;
    }

    return EXIT_SUCCESS;
}
