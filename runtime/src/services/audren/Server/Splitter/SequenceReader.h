// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Port of the System.Buffers.SequenceReader<byte> use in Ryujinx.Audio (splitter and mix input parsing) over one contiguous span.
// Guest memory is host memory here, so there are no segments. The helpers follow Ryujinx.Common/Extensions/SequenceReaderExtensions.cs:
//   * GetRefOrRefToCopy<T>(): consumes sizeof(T) bytes and returns the value. C# returns a ref into the buffer; this returns a copy,
//     because the bytes may be unaligned. Callers only read it, so the behaviour is the same.
//   * ReadLittleEndian(int32_t&): consumes 4 bytes. C# throws when the data is short; here a short read is fatal (audren_common.h).
//   * Rewind / Advance / SetConsumed: as on SequenceReader. SetConsumed is an absolute position (SequenceReaderExtensions.SetConsumed).
// Shared helper: any other port that parses the user input buffer can use this class.
#pragma once
#include <cstring>
#include <type_traits>

#include "../../audren_common.h"

namespace nxrt::audren {

class SequenceReader {
  public:
    explicit SequenceReader(Span<const uint8_t> data) : _data(data) {}

    // C# SequenceReader.Consumed: bytes consumed so far.
    int64_t Consumed() const { return _consumed; }

    // Bytes not consumed yet.
    int64_t Remaining() const { return static_cast<int64_t>(_data.Length) - _consumed; }

    // C# GetRefOrRefToCopy<T>(out _): reads one T and advances by sizeof(T).
    template <class T>
    T GetRefOrRefToCopy() {
        static_assert(std::is_trivially_copyable_v<T>, "unmanaged");

        if (Remaining() < static_cast<int64_t>(sizeof(T))) {
            NXAUDREN_FATAL("SequenceReader: not enough data for the requested value");
        }

        T value{};
        std::memcpy(static_cast<void*>(&value), _data.Data + _consumed, sizeof(T));
        _consumed += static_cast<int64_t>(sizeof(T));
        return value;
    }

    // C# ReadLittleEndian(out int): reads 4 little-endian bytes and advances by 4.
    void ReadLittleEndian(int32_t& value) {
        if (Remaining() < 4) {
            NXAUDREN_FATAL("SequenceReader: not enough data for an int");
        }

        const uint8_t* p = _data.Data + _consumed;
        uint32_t raw = static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) | (static_cast<uint32_t>(p[2]) << 16) |
                       (static_cast<uint32_t>(p[3]) << 24);
        value = static_cast<int32_t>(raw);
        _consumed += 4;
    }

    // C# SequenceReader.Advance.
    void Advance(int64_t count) {
        _consumed += count;
        NXAUDREN_ASSERT(_consumed <= static_cast<int64_t>(_data.Length));
    }

    // C# SequenceReader.Rewind.
    void Rewind(int64_t count) {
        _consumed -= count;
        NXAUDREN_ASSERT(_consumed >= 0);
    }

    // C# SequenceReaderExtensions.SetConsumed: moves the cursor to an absolute position.
    void SetConsumed(int64_t consumed) {
        _consumed = consumed;
        NXAUDREN_ASSERT(_consumed >= 0 && _consumed <= static_cast<int64_t>(_data.Length));
    }

  private:
    // The whole input buffer.
    Span<const uint8_t> _data;
    // Bytes consumed so far.
    int64_t _consumed = 0;
};

}  // namespace nxrt::audren
