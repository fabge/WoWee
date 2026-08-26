#pragma once

#include <vector>
#include <cstdint>
#include <string>

namespace wowee {
namespace network {

class Packet {
public:
    Packet() = default;
    explicit Packet(uint16_t opcode);
    Packet(uint16_t opcode, const std::vector<uint8_t>& data);
    Packet(uint16_t opcode, std::vector<uint8_t>&& data);

    void writeUInt8(uint8_t value);
    void writeUInt16(uint16_t value);
    void writeUInt32(uint32_t value);
    void writeUInt64(uint64_t value);
    void writeFloat(float value);
    void writeString(const std::string& value);
    void writeBytes(const uint8_t* data, size_t length);

    uint8_t readUInt8();
    uint16_t readUInt16();
    uint32_t readUInt32();
    uint64_t readUInt64();
    float readFloat();
    uint64_t readPackedGuid();
    void writePackedGuid(uint64_t guid);
    std::string readString();

    /// A name written as a 32-bit length followed by that many bytes, the
    /// length counting a trailing null the server includes.
    ///
    /// Three chat parsers read this by hand and only one of them checked the
    /// length against what the packet actually had left. Without that check a
    /// truncated packet yields a name padded out of thin air - readUInt8
    /// answers zero past the end rather than failing - and every field after
    /// it is read from beyond the data too. The name comes back plausible and
    /// the target guid comes back nothing.
    ///
    /// False means the length is not one this packet can honour, and the read
    /// position is left where it was.
    // [[nodiscard]] because the whole point of this helper is the bool: on a
    // truncated packet it restores the read position and reports false, so a
    // caller that drops the result reads the length field it just rejected as
    // the next field's bytes and carries on parsing garbage that looks valid.
    // Three chat parsers did exactly that.
    [[nodiscard]] bool readSizedString(std::string& out, uint32_t maxLength = 256);

    [[nodiscard]] uint16_t getOpcode() const { return opcode; }
    [[nodiscard]] const std::vector<uint8_t>& getData() const { return data; }
    [[nodiscard]] size_t getReadPos() const { return readPos; }
    [[nodiscard]] size_t getSize() const { return data.size(); }
    // Clamp to 0 instead of wrapping to ~(size_t)0 when readPos overshoots
    // (can happen via setReadPos with an unchecked offset).
    [[nodiscard]] size_t getRemainingSize() const { return (readPos <= data.size()) ? (data.size() - readPos) : 0; }
    [[nodiscard]] bool hasRemaining(size_t need) const { return readPos <= data.size() && need <= (data.size() - readPos); }

    /// A server-supplied element count, clamped to what this packet could
    /// actually still contain.
    ///
    /// For reserving before a parse loop. The loops themselves are already
    /// written to stop when the bytes run out, so the count never decided how
    /// much was *read* - but it did decide how much was *allocated*, and a
    /// count of 0xFFFFFFFF in a twelve-byte packet asked for an allocation of
    /// several gigabytes and took the client down with bad_alloc. A malformed
    /// packet must cost a rejected packet, not the process.
    ///
    /// `entryBytes` is the smallest number of bytes one element can occupy.
    /// One is the honest default for a variable-length entry and is still a
    /// hard bound, because nothing can appear more often than once per byte.
    [[nodiscard]] size_t boundedCount(uint64_t count, size_t entryBytes = 1) const {
        const size_t capacity = getRemainingSize() / (entryBytes ? entryBytes : 1);
        return (count < capacity) ? static_cast<size_t>(count) : capacity;
    }
    [[nodiscard]] bool hasFullPackedGuid() const {
        if (readPos >= data.size()) return false;
        uint8_t mask = data[readPos];
        size_t guidBytes = 1;
        for (int bit = 0; bit < 8; ++bit)
            if (mask & (1u << bit)) ++guidBytes;
        return getRemainingSize() >= guidBytes;
    }
    void setReadPos(size_t pos) { readPos = pos; }
    [[nodiscard]] bool hasData() const { return readPos < data.size(); }
    void skipAll() { readPos = data.size(); }

private:
    uint16_t opcode = 0;
    std::vector<uint8_t> data;
    size_t readPos = 0;
};

} // namespace network
} // namespace wowee
