#include "Version.h"
#include "minimap.h"
#include "utils/bam_utils.h"
#include "utils/log_utils.h"

#include <argparse.hpp>
#include <spdlog/spdlog.h>

#include <string>
#include <thread>
#include <vector>

#ifndef _WIN32
#include <unistd.h>
#endif

namespace dorado {

int aligner(int argc, char* argv[]) {
    utils::InitLogging();

    argparse::ArgumentParser parser("dorado", DORADO_VERSION, argparse::default_arguments::help);
    parser.add_argument("index").help("reference in (fastq/fasta/mmi).");
    parser.add_argument("reads").help("any HTS format.").nargs(argparse::nargs_pattern::any);
    parser.add_argument("-t", "--threads").default_value(0).scan<'i', int>();
    parser.add_argument("-n", "--max-reads").default_value(1000).scan<'i', int>();
    parser.add_argument("-v", "--verbose").default_value(false).implicit_value(true);

    try {
        parser.parse_args(argc, argv);
    } catch (const std::exception& e) {
        std::ostringstream parser_stream;
        parser_stream << parser;
        spdlog::error("{}\n{}", e.what(), parser_stream.str());
        std::exit(1);
    }

    if (parser.get<bool>("--verbose")) {
        mm_verbose = 3;
        spdlog::set_level(spdlog::level::debug);
    }

    auto index(parser.get<std::string>("index"));
    auto reads(parser.get<std::vector<std::string>>("reads"));
    auto threads(parser.get<int>("threads"));
    auto max_reads(parser.get<int>("max-reads"));

    threads = threads == 0 ? std::thread::hardware_concurrency() : threads;
    spdlog::debug("> threads {}", threads);

    if (reads.size() == 0) {
#ifndef _WIN32
        if (isatty(fileno(stdin))) {
            std::cout << parser << std::endl;
            return 1;
        }
#endif
        reads.push_back("-");
    } else if (reads.size() > 1) {
        spdlog::error("> multi file input not yet handled");
        return 1;
    }

    spdlog::info("> loading index {}", index);
    utils::Aligner aligner(index, threads);
    spdlog::info("> loaded index {}", index);

    utils::BamReader reader(reads[0]);
    utils::BamWriter writer("-", reader.m_header, aligner.get_idx_records());

    spdlog::debug("> input fmt: {} aligned: {}", reader.m_format, reader.m_is_aligned);

    spdlog::info("> starting alignment");
    int num_reads = 0;
    while (reader.read()) {
        auto records = aligner.align(reader.m_record);
        for (auto record : records) {
            writer.write(record);
        }
        if (num_reads++ >= max_reads)
            break;
    }
    spdlog::info("> finished alignment");

    return 0;
}

}  // namespace dorado
