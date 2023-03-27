#pragma once
#include "htslib/sam.h"
#include "minimap.h"
#include "read_pipeline/ReadPipeline.h"

#include <map>
#include <set>
#include <string>

using sq_t = std::vector<std::pair<char*, uint32_t>>;

namespace dorado::utils {

/**
 * @brief Reads a SAM/BAM/CRAM file and returns a map of read IDs to Read objects.
 *
 * This function opens a SAM/BAM/CRAM file specified by the input filename parameter,
 * reads the alignments, and creates a map that associates read IDs with their
 * corresponding Read objects. The Read objects contain the read ID, sequence,
 * and quality string.
 *
 * @param filename The input BAM file path as a string.
 * @param read_ids A set of read_ids to filter on.
 * @return A map with read IDs as keys and shared pointers to Read objects as values.
 *
 * @note The caller is responsible for managing the memory of the returned map.
 * @note The input BAM file must be properly formatted and readable.
 */
std::map<std::string, std::shared_ptr<Read>> read_bam(const std::string& filename,
                                                      const std::set<std::string>& read_ids);

class Aligner {
public:
    Aligner(const std::string& filename, int threads);
    ~Aligner();
    sq_t get_idx_records();
    std::vector<bam1_t*> align(bam1_t* record);
    std::pair<int, mm_reg1_t*> align(const std::vector<char> seq);

private:
    int m_threads = 1;
    mm_idxopt_t m_idx_opt;
    mm_mapopt_t m_map_opt;
    mm_idx_t* m_index;
    mm_idx_reader_t* m_index_reader;
    std::vector<mm_tbuf_t*> m_tbufs;
};

class BamReader {
public:
    BamReader(const std::string& filename);
    ~BamReader();
    bool read();
    char* m_format;
    bool m_is_aligned;
    bam1_t* m_record;
    sam_hdr_t* m_header;

private:
    htsFile* m_file;
};

class BamWriter {
public:
    BamWriter(const std::string& filename, const sam_hdr_t* header, const sq_t seqs);
    ~BamWriter();
    int write(bam1_t* record);
    sam_hdr_t* m_header;

private:
    htsFile* m_file;
    int write_hdr_pg();
    int write_hdr_sq(char* name, uint32_t length);
};

}  // namespace dorado::utils
