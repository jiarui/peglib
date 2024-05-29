#pragma once
#include <filesystem>
#include <string>
#include <fstream>
namespace peg
{
    struct FileReader {
        FileReader(size_t buffer_size, const std::string& path) : m_bufsize{buffer_size}, m_fstream(path) {
            m_filesize = std::filesystem::file_size(path);
        }
        using IterType = size_t;
        bool ended(IterType current) {
            return current == m_filesize;
        }
        
    protected:
        size_t m_bufsize;
        size_t m_filesize;
        std::ifstream m_fstream;
    };
    
} // namespace peg
