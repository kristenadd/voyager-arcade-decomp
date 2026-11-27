#pragma once
#include <cstdint>
#include <cstdio>
#include <string>

namespace voy::host {

class Process;

// printf: `fmt` and the varargs (`args` is the guest address of the first one).
std::string guest_vformat(Process& p, std::uint32_t fmt, std::uint32_t args);

// Where scanf reads from.
class ScanSource {
public:
    virtual ~ScanSource() = default;
    // One literal-plus-conversion piece; `target` null when suppressed. Returns
    // the host scanf result (1, 0 or EOF).
    virtual int scan_one(const std::string& fmt, void* target) = 0;
    virtual void scan_literal(const std::string& lit) = 0;
    virtual std::size_t consumed() const = 0;
};

class StringScanSource : public ScanSource {
public:
    explicit StringScanSource(std::string s) : s_(std::move(s)) {}
    int scan_one(const std::string& fmt, void* target) override;
    void scan_literal(const std::string& lit) override;
    std::size_t consumed() const override { return pos_; }

private:
    std::string s_;
    std::size_t pos_ = 0;
};

class FileScanSource : public ScanSource {
public:
    explicit FileScanSource(FILE* fp) : fp_(fp) {}
    int scan_one(const std::string& fmt, void* target) override;
    void scan_literal(const std::string& lit) override;
    std::size_t consumed() const override { return consumed_; }

private:
    FILE* fp_;
    std::size_t consumed_ = 0;
};

// scanf: returns the number of assigned conversions, or EOF.
int guest_vscan(Process& p, ScanSource& src, std::uint32_t fmt, std::uint32_t args);

}
