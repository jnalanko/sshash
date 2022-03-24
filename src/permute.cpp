#include <iostream>

#include "../include/gz/zip_stream.cpp"
#include "../external/pthash/external/essentials/include/essentials.hpp"
#include "../external/pthash/external/cmd_line_parser/include/parser.hpp"
#include "../include/builder/cover.hpp"
#include "../include/builder/util.hpp"

using namespace sshash;

struct permute_data {
    permute_data() : num_runs_abundances(0), num_sequences(0) {}
    uint64_t num_runs_abundances;
    uint64_t num_sequences;
    std::vector<vertex> vertices;
};

void parse_file(std::istream& is, permute_data& data, build_configuration const& build_config) {
    std::string sequence;
    uint64_t k = build_config.k;
    uint64_t num_bases = 0;
    uint64_t num_kmers = 0;
    uint64_t seq_len = 0;

    uint64_t sum_of_abundances = 0;
    uint64_t num_sequences_diff_abs = 0;  // num sequences whose kmers have different abundances
    uint64_t num_sequences_all_mfa = 0;   // num sequences whose kmers have same abundance == mfa
    data.num_runs_abundances = 0;
    data.num_sequences = 0;

    auto parse_header = [&]() {
        if (sequence.empty()) return;

        /*
            Heder format:
            >[id] LN:i:[seq_len] ab:Z:[ab_seq]
            where [ab_seq] is a space-separated sequence of integer counters (the abundances),
            whose length is equal to [seq_len]-k+1
        */

        // example header: '>12 LN:i:41 ab:Z:2 2 2 2 2 2 2 2 2 2 2'

        expect(sequence[0], '>');
        uint64_t i = 0;
        i = sequence.find_first_of(' ', i);
        if (i == std::string::npos) throw parse_runtime_error();

        i += 1;
        expect(sequence[i + 0], 'L');
        expect(sequence[i + 1], 'N');
        expect(sequence[i + 2], ':');
        expect(sequence[i + 3], 'i');
        expect(sequence[i + 4], ':');
        i += 5;
        uint64_t j = sequence.find_first_of(' ', i);
        if (j == std::string::npos) throw parse_runtime_error();

        seq_len = std::strtoull(sequence.data() + i, nullptr, 10);
        i = j + 1;
        expect(sequence[i + 0], 'a');
        expect(sequence[i + 1], 'b');
        expect(sequence[i + 2], ':');
        expect(sequence[i + 3], 'Z');
        expect(sequence[i + 4], ':');
        i += 5;

        bool kmers_have_all_mfa = true;
        bool kmers_have_different_abundances = false;
        uint64_t front = constants::invalid;
        uint64_t back = constants::invalid;

        for (uint64_t j = 0, prev_abundance = constants::invalid; j != seq_len - k + 1; ++j) {
            uint64_t abundance = std::strtoull(sequence.data() + i, nullptr, 10);
            sum_of_abundances += abundance;
            i = sequence.find_first_of(' ', i) + 1;

            /* set front and back */
            if (j == 0) front = abundance;
            if (j == seq_len - k) back = abundance;

            /* accumulate statistics */
            if (abundance != constants::most_frequent_abundance) kmers_have_all_mfa = false;
            if (j > 0 and abundance != prev_abundance) kmers_have_different_abundances = true;

            /* count the number of runs */
            if (abundance != prev_abundance) data.num_runs_abundances += 1;

            prev_abundance = abundance;
        }

        num_sequences_diff_abs += kmers_have_different_abundances;
        num_sequences_all_mfa += kmers_have_all_mfa;

        data.vertices.emplace_back(data.num_sequences, front, back);
    };

    while (!is.eof()) {
        std::getline(is, sequence);  // header sequence
        parse_header();

        std::getline(is, sequence);  // DNA sequence
        if (sequence.size() < k) continue;

        if (++data.num_sequences % 100000 == 0) {
            std::cout << "read " << data.num_sequences << " sequences, " << num_bases << " bases, "
                      << num_kmers << " kmers" << std::endl;
        }

        num_bases += sequence.size();
        num_kmers += sequence.size() - k + 1;

        if (seq_len != sequence.size()) {
            std::cout << "ERROR: expected a sequence of length " << seq_len
                      << " but got one of length " << sequence.size() << std::endl;
            throw std::runtime_error("file is malformed");
        }
    }

    assert(data.vertices.size() == data.num_sequences);
    assert(data.num_runs_abundances >= data.num_sequences);

    std::cout << "read " << data.num_sequences << " sequences, " << num_bases << " bases, "
              << num_kmers << " kmers" << std::endl;
    std::cout << "sum_of_abundances " << sum_of_abundances << std::endl;
    std::cout << "num_sequences whose kmers have different abundances: " << num_sequences_diff_abs
              << "/" << data.num_sequences << " ("
              << (num_sequences_diff_abs * 100.0) / data.num_sequences << "%)" << std::endl;
    std::cout << "num_sequences whose kmers all have the same abundance != mfa: "
              << (data.num_sequences - num_sequences_diff_abs) << "/" << data.num_sequences << " ("
              << ((data.num_sequences - num_sequences_diff_abs) * 100.0) / data.num_sequences
              << "%)" << std::endl;
    std::cout << "num_sequences whose kmers all have the same abundance == mfa: "
              << num_sequences_all_mfa << "/" << (data.num_sequences - num_sequences_diff_abs)
              << " ("
              << (num_sequences_all_mfa * 100.0) / (data.num_sequences - num_sequences_diff_abs)
              << "%)" << std::endl;
}

permute_data parse_file(std::string const& filename, build_configuration const& build_config) {
    std::ifstream is(filename.c_str());
    if (!is.good()) throw std::runtime_error("error in opening the file '" + filename + "'");
    std::cout << "reading file '" << filename << "'..." << std::endl;
    permute_data data;
    if (util::ends_with(filename, ".gz")) {
        zip_istream zis(is);
        parse_file(zis, data, build_config);
    } else {
        parse_file(is, data, build_config);
    }
    is.close();
    return data;
}

void permute_and_write(std::istream& is, std::string const& output_filename,
                       pthash::compact_vector const& permutation) {
    // std::vector<std::string> filenames;
    // constexpr uint64_t bytes = 2 * essentials::GB;
    std::vector<std::pair<std::string, std::string>> buffer;  // (header, dna)

    std::string header_sequence;
    std::string dna_sequence;
    uint64_t num_sequences = permutation.size();
    uint64_t num_bases = 0;

    for (uint64_t i = 0; i != num_sequences; ++i) {
        std::getline(is, header_sequence);
        std::getline(is, dna_sequence);
        buffer.emplace_back(header_sequence, dna_sequence);

        num_bases += dna_sequence.size();

        // check for bytes used and flush

        if (i != 0 and i % 100000 == 0) {
            std::cout << "read " << i << " sequences, " << num_bases << " bases" << std::endl;
        }
    }

    std::cout << "read " << num_sequences << " sequences, " << num_bases << " bases" << std::endl;

    std::cout << "sorting..." << std::endl;
    std::sort(buffer.begin(), buffer.end(), [&](auto const& p_x, auto const& p_y) {
        assert(p_x.first.front() == '>');
        assert(p_y.first.front() == '>');
        uint64_t seq_id_x = std::strtoull(p_x.first.data() + 1, nullptr, 10);
        uint64_t seq_id_y = std::strtoull(p_y.first.data() + 1, nullptr, 10);
        return permutation[seq_id_x] < permutation[seq_id_y];
    });

    std::cout << "saving to file '" << output_filename << "'..." << std::endl;
    std::ofstream out(output_filename.c_str());
    for (auto const& seq : buffer) out << seq.first << '\n' << seq.second << '\n';
    out.close();
}

void permute_and_write(std::string const& input_filename, std::string const& output_filename,
                       pthash::compact_vector const& permutation) {
    std::ifstream is(input_filename.c_str());
    if (!is.good()) throw std::runtime_error("error in opening the file '" + input_filename + "'");
    std::cout << "reading file '" << input_filename << "'..." << std::endl;
    if (util::ends_with(input_filename, ".gz")) {
        zip_istream zis(is);
        permute_and_write(zis, output_filename, permutation);
    } else {
        permute_and_write(is, output_filename, permutation);
    }
    is.close();
}

int main(int argc, char** argv) {
    cmd_line_parser::parser parser(argc, argv);

    /* mandatory arguments */
    parser.add("input_filename",
               "Must be a FASTA file (.fa/fasta extension) compressed with gzip (.gz) or not:\n"
               "\t- without duplicate nor invalid kmers\n"
               "\t- one DNA sequence per line\n"
               "\t- with also kmers' abundances.\n"
               "\tFor example, it could be the de Bruijn graph topology output by BCALM.");

    parser.add("k", "K-mer length (must be <= " + std::to_string(constants::max_k) + ").");

    /* optional arguments */
    parser.add("output_filename", "Output file where the permuted collection will be written.",
               "-o", false);
    // parser.add("verbose", "Verbose output during construction.", "--verbose", true);

    if (!parser.parse()) return 1;

    auto input_filename = parser.get<std::string>("input_filename");
    auto k = parser.get<uint64_t>("k");

    build_configuration build_config;
    build_config.k = k;
    // build_config.verbose = parser.get<bool>("verbose");

    std::string output_filename = input_filename + ".permuted";
    if (parser.parsed("output_filename")) {
        output_filename = parser.get<std::string>("output_filename");
    }

    std::string permutation_filename = input_filename + ".permutation";

    auto data = parse_file(input_filename, build_config);

    {
        uint64_t R_lower = data.num_runs_abundances - data.vertices.size() + 1;
        std::cout << "The trivial lower bound (too optimistic) assumes we are able to concatenate "
                     "all sequences : R_lo = "
                  << R_lower << std::endl;

        std::sort(data.vertices.begin(), data.vertices.end(), [](auto const& x, auto const& y) {
            /* sort on front */
            if (x.front != y.front) return x.front < y.front;
            if (x.back != y.back) return x.back < y.back;
            return x.id < y.id;
        });

        /* (abundance, num_seqs_with_front=abundance) */
        // We assume there are less than 2^32 sequences and that
        // the largest abundance fits into a 32-bit uint.
        std::unordered_map<uint32_t, uint32_t> abundance_map;

        uint64_t prev_front = data.vertices.front().front;
        uint64_t count = 0;
        for (auto const& vertex : data.vertices) {
            if (vertex.front != prev_front) {
                abundance_map[prev_front] = count;
                count = 0;
            }
            count += 1;
            prev_front = vertex.front;
        }
        abundance_map[prev_front] = count;

        uint64_t R = data.num_runs_abundances;
        for (auto const& vertex : data.vertices) {
            uint64_t back = vertex.back;
            auto it = abundance_map.find(back);
            if (it != abundance_map.cend()) {  // found
                if ((*it).second > 0) {        // if it is 0, we cannot find a match

                    /* We clearly cannot create more mergings than num_sequences - 1,
                       thus R cannot be lower than R_lower. */
                    if (R == R_lower) break;

                    (*it).second -= 1;
                    R -= 1;
                }
            }
        }
        std::cout << "Computed lower bound: R_hi = " << R << std::endl;
    }

    {
        /* compute cover */
        cover c(data.num_sequences, data.num_runs_abundances);
        assert(data.vertices.size() == data.num_sequences);
        c.compute(data.vertices);
        c.save(permutation_filename);
        std::vector<vertex>().swap(data.vertices);
    }

    /* permute */
    pthash::compact_vector permutation;
    {
        std::ifstream is(permutation_filename.c_str());
        if (!is.good()) {
            throw std::runtime_error("error in opening the file '" + permutation_filename + "'");
        }
        pthash::compact_vector::builder cv_builder(data.num_sequences,
                                                   std::ceil(std::log2(data.num_sequences)));
        for (uint64_t i = 0; i != data.num_sequences; ++i) {
            uint64_t position = 0;
            is >> position;
            cv_builder.set(position, i);
        }
        is.close();
        cv_builder.build(permutation);
    }

    permute_and_write(input_filename, output_filename, permutation);
    std::remove(permutation_filename.c_str());

    return 0;
}
