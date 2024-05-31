#pragma once
#include <filesystem>
#include <string>
#include <cstdio>
#include <optional>
namespace peg
{
    struct FileSource {
        FileSource(size_t buffer_size, const std::string& path) : m_current_buf{0}, m_buffer_size(buffer_size)
        {
            int remider = buffer_size % sizeof(value_type);
            if(remider == 0) {
                m_buffer_size = buffer_size;
            }
            else {
                m_buffer_size = buffer_size + sizeof(value_type) - remider;
            }
            m_fp = fopen(path.data(), "r");
            m_filesize = std::filesystem::file_size(path);
            auto n = m_bufs[0].read(m_fp, m_buffer_size);
            if(n == m_buffer_size){
                m_bufs[1].read(m_fp, m_buffer_size);
            }
        }
        ~FileSource() {
            fclose(m_fp);
        }

        using value_type = char;

        struct iterator;

        value_type get(iterator i) {
            auto v = m_bufs[m_current_buf].get(i);
            if(v){
                return v.value();
            }
            m_current_buf = !m_current_buf;
            v = m_bufs[m_current_buf].get(i);
            if(v) {
                return v.value();
            }
            bool ret = read_to(i.m_pos, m_current_buf);
            if(ret) {
                return m_bufs[m_current_buf].get(i).value();
            }
            else {
                return -1;
            }
        }

        

        struct iterator {
            bool operator<(const iterator& rhs){
                return m_freader == rhs.m_freader && m_pos < rhs.m_pos;
            }
            bool operator==(const iterator& rhs){
                return m_freader == rhs.m_freader && m_pos == rhs.m_pos;
            }

            iterator& operator++() {
                m_pos++;
                return *this;
            }

            iterator operator++(int) {
                return iterator(m_freader, m_pos+1);
            }

            typename FileSource::value_type operator*() {
                return m_freader->get(*this);
            }
            friend FileSource;
        protected:
            iterator(FileSource* f) : m_freader{f}, m_pos{0} {};
            iterator(FileSource* f, size_t pos) : m_freader{f}, m_pos{pos} {};
            size_t m_pos; // in item index
            FileSource* m_freader;
        };
        

        iterator end() {
            return {this, m_filesize};
        }
        iterator begin() {
            return {this, 0};
        }
        
    protected:
        struct buffer {
            std::vector<value_type> m_buf;
            size_t m_buf_from;
            size_t m_buf_to;
            size_t read(FILE* fp, size_t n) {
                m_buf.reserve(sizeof(value_type)* n);
                m_buf_from = ftell(fp) / sizeof(value_type);
                size_t read_items = fread(m_buf.data(), sizeof(value_type), n, fp);
                m_buf_to = m_buf_from + read_items;
                return read_items;
            }

            bool in(iterator i) {
                return i.m_pos >= m_buf_from && i.m_pos < m_buf_to;
            }

            std::optional<value_type> get(iterator i) {
                if(in(i)){
                    return m_buf[i.m_pos - m_buf_from];
                }
                else {
                    return std::nullopt;
                }
            }
        };
        size_t m_filesize;
        size_t m_buffer_size; // in items number
        buffer m_bufs[2];
        int m_current_buf;
        FILE *m_fp;

        bool read_to(size_t item_pos, int index) {
            if(item_pos >= m_filesize/sizeof(value_type)){
                return false;
            }
            size_t buf_pos = item_pos - (item_pos % (m_buffer_size / sizeof(value_type)));
            int ret = fseek(m_fp, buf_pos * sizeof(value_type), SEEK_SET);
            if (ret != 0) {
                return false;
            }
            ret = m_bufs[index].read(m_fp, m_buffer_size);
            return ret > 0;
        }

    };
    
} // namespace peg
