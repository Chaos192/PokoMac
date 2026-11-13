// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Port of Ryujinx.Audio/Renderer/Common/EdgeMatrix.cs. An adjacency matrix stored as bits (used for splitter routing).
#pragma once
#include "../audren_common.h"
#include "../Constants.h"
#include "../Utils/BitArray.h"
#include "../Utils/BitUtils.h"

namespace nxrt::audren {

class EdgeMatrix {
public:
    // Get the required work buffer size memory needed for the EdgeMatrix.
    static int32_t GetWorkBufferSize(int32_t nodeCount) {
        int32_t size = BitUtils::AlignUp(nodeCount * nodeCount, Constants::BufferAlignment);
        return size / static_cast<int32_t>(sizeof(uint8_t));
    }

    // Initializes the EdgeMatrix instance with backing memory.
    void Initialize(Span<uint8_t> edgeMatrixWorkBuffer, int32_t nodeCount) {
        NXAUDREN_ASSERT(static_cast<int64_t>(edgeMatrixWorkBuffer.Length) >= GetWorkBufferSize(nodeCount));

        _storage = BitArray(edgeMatrixWorkBuffer);

        _nodeCount = nodeCount;

        _storage.Reset();
    }

    // Test if the bit at the given index is set.
    bool Test(int32_t index) const { return _storage.Test(index); }

    // Reset all bits in the storage.
    void Reset() { _storage.Reset(); }

    // Reset the bit at the given index.
    void Reset(int32_t index) { _storage.Reset(index); }

    // Set the bit at the given index.
    void Set(int32_t index) { _storage.Set(index); }

    // Connect a given source to a given destination.
    void Connect(int32_t source, int32_t destination) {
        NXAUDREN_ASSERT(source < _nodeCount);
        NXAUDREN_ASSERT(destination < _nodeCount);

        _storage.Set(_nodeCount * source + destination);
    }

    // Check if the given source is connected to the given destination.
    bool Connected(int32_t source, int32_t destination) const {
        NXAUDREN_ASSERT(source < _nodeCount);
        NXAUDREN_ASSERT(destination < _nodeCount);

        return _storage.Test(_nodeCount * source + destination);
    }

    // Disconnect a given source from a given destination.
    void Disconnect(int32_t source, int32_t destination) {
        NXAUDREN_ASSERT(source < _nodeCount);
        NXAUDREN_ASSERT(destination < _nodeCount);

        _storage.Reset(_nodeCount * source + destination);
    }

    // Remove all edges from a given source.
    void RemoveEdges(int32_t source) {
        for (int32_t i = 0; i < _nodeCount; i++) {
            Disconnect(source, i);
        }
    }

    // Get the total node count.
    int32_t GetNodeCount() const { return _nodeCount; }

private:
    // Backing BitArray used for node connections.
    BitArray _storage;

    // The count of nodes of the current instance.
    int32_t _nodeCount = 0;
};

}  // namespace nxrt::audren
