#pragma once
#include <algorithm>
#include <cctype>
#include <cstddef>
#include <cstdint>
#include <fstream>
#include <initializer_list>
#include <optional>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "core/instructions.hpp"
#include "core/addr.hpp"
#include "core/types.hpp"
#include "core/registers.hpp"

struct OutputStreamToCSV{
    // treat the external file as a register
    std::string filename;
    std::vector< ... > write_buffer; //wire

    explicit OutputStreamToCSV(std::string name) : filename(std::move(name)) {}

    void reset  ();// open file
    void prepare();// clear all wires, that is, buffers
    void commit ();// output data to file

    //size_t get_ready_num() const {    } // for static_length record, not needed for now

    void record(size_t valid_num, std::vector<CanonicalEvent>& events) {
        // record needed informations into buffer...
    }
};
