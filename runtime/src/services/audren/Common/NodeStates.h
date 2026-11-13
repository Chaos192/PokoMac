// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Port of Ryujinx.Audio/Renderer/Common/NodeStates.cs. Depth-first topological sort over an EdgeMatrix with loop detection.
#pragma once
#include <cstddef>

#include "../audren_common.h"
#include "EdgeMatrix.h"

namespace nxrt::audren {

class NodeStates {
private:
    // Int stack over a caller-provided span (C# private class Stack).
    class Stack {
    public:
        void Reset(Span<int32_t> storage, int32_t nodeCount) {
            NXAUDREN_ASSERT(static_cast<int64_t>(storage.Length) * static_cast<int64_t>(sizeof(int32_t)) >=
                            static_cast<int64_t>(CalcBufferSize(nodeCount)));

            _storage = storage;
            _index = 0;
            _nodeCount = nodeCount;
        }

        int32_t GetCurrentCount() const { return _index; }

        void Push(int32_t data) {
            NXAUDREN_ASSERT(_index + 1 <= _nodeCount);

            _storage[static_cast<size_t>(_index++)] = data;
        }

        int32_t Pop() {
            NXAUDREN_ASSERT(_index > 0);

            return _storage[static_cast<size_t>(--_index)];
        }

        int32_t Top() const { return _storage[static_cast<size_t>(_index - 1)]; }

        static int32_t CalcBufferSize(int32_t nodeCount) { return nodeCount * static_cast<int32_t>(sizeof(int32_t)); }

    private:
        Span<int32_t> _storage;
        int32_t _index = 0;
        int32_t _nodeCount = 0;
    };

    enum class NodeState : uint8_t {
        Unknown,
        Discovered,
        Finished,
    };

public:
    NodeStates() = default;

    static int32_t GetWorkBufferSize(int32_t nodeCount) {
        return Stack::CalcBufferSize(nodeCount * nodeCount) + 0xC * nodeCount + 2 * EdgeMatrix::GetWorkBufferSize(nodeCount);
    }

    void Initialize(Span<uint8_t> nodeStatesWorkBuffer, int32_t nodeCount) {
        [[maybe_unused]] int32_t workBufferSize = GetWorkBufferSize(nodeCount);

        NXAUDREN_ASSERT(static_cast<int64_t>(nodeStatesWorkBuffer.Length) >= static_cast<int64_t>(workBufferSize));

        _nodeCount = nodeCount;

        int32_t edgeMatrixWorkBufferSize = EdgeMatrix::GetWorkBufferSize(nodeCount);

        _discovered.Initialize(nodeStatesWorkBuffer.Slice(0, static_cast<size_t>(edgeMatrixWorkBufferSize)), nodeCount);
        _finished.Initialize(nodeStatesWorkBuffer.Slice(static_cast<size_t>(edgeMatrixWorkBufferSize),
                                                        static_cast<size_t>(edgeMatrixWorkBufferSize)),
                             nodeCount);

        nodeStatesWorkBuffer = nodeStatesWorkBuffer.Slice(static_cast<size_t>(edgeMatrixWorkBufferSize) * 2);

        _resultArray = nodeStatesWorkBuffer.Slice(0, static_cast<size_t>(sizeof(int32_t) * nodeCount)).Cast<int32_t>();

        nodeStatesWorkBuffer = nodeStatesWorkBuffer.Slice(static_cast<size_t>(sizeof(int32_t) * nodeCount));

        Span<int32_t> stackWorkBuffer =
            nodeStatesWorkBuffer.Slice(0, static_cast<size_t>(Stack::CalcBufferSize(nodeCount * nodeCount))).Cast<int32_t>();

        _stack.Reset(stackWorkBuffer, nodeCount * nodeCount);
    }

    // Returns the sorted node ids (valid after Sort returned true).
    Span<const int32_t> GetTsortResult() const {
        return _resultArray.Slice(0, static_cast<size_t>(_tsortResultIndex));
    }

    bool Sort(const EdgeMatrix& edgeMatrix) {
        Reset();

        if (_nodeCount <= 0) {
            return true;
        }

        for (int32_t i = 0; i < _nodeCount; i++) {
            if (GetState(i) == NodeState::Unknown) {
                _stack.Push(i);
            }

            while (_stack.GetCurrentCount() > 0) {
                int32_t topIndex = _stack.Top();

                NodeState topState = GetState(topIndex);

                if (topState == NodeState::Discovered) {
                    SetState(topIndex, NodeState::Finished);
                    PushTsortResult(topIndex);
                    _stack.Pop();
                } else if (topState == NodeState::Finished) {
                    _stack.Pop();
                } else {
                    if (topState == NodeState::Unknown) {
                        SetState(topIndex, NodeState::Discovered);
                    }

                    for (int32_t j = 0; j < edgeMatrix.GetNodeCount(); j++) {
                        if (edgeMatrix.Connected(topIndex, j)) {
                            NodeState jState = GetState(j);

                            if (jState == NodeState::Unknown) {
                                _stack.Push(j);
                            }
                            // Found a loop, reset and propagate rejection.
                            else if (jState == NodeState::Discovered) {
                                Reset();

                                return false;
                            }
                        }
                    }
                }
            }
        }

        return true;
    }

private:
    void Reset() {
        _discovered.Reset();
        _finished.Reset();
        _tsortResultIndex = 0;
        _resultArray.Fill(-1);
    }

    NodeState GetState(int32_t index) const {
        NXAUDREN_ASSERT(index < _nodeCount);

        if (_discovered.Test(index)) {
            NXAUDREN_ASSERT(!_finished.Test(index));

            return NodeState::Discovered;
        }

        if (_finished.Test(index)) {
            NXAUDREN_ASSERT(!_discovered.Test(index));

            return NodeState::Finished;
        }

        return NodeState::Unknown;
    }

    void SetState(int32_t index, NodeState state) {
        switch (state) {
            case NodeState::Unknown:
                _discovered.Reset(index);
                _finished.Reset(index);
                break;
            case NodeState::Discovered:
                _discovered.Set(index);
                _finished.Reset(index);
                break;
            case NodeState::Finished:
                _finished.Set(index);
                _discovered.Reset(index);
                break;
        }
    }

    void PushTsortResult(int32_t index) {
        NXAUDREN_ASSERT(index < _nodeCount);

        _resultArray[static_cast<size_t>(_tsortResultIndex++)] = index;
    }

    int32_t _nodeCount = 0;
    EdgeMatrix _discovered;
    EdgeMatrix _finished;
    Span<int32_t> _resultArray;
    Stack _stack;
    int32_t _tsortResultIndex = 0;
};

}  // namespace nxrt::audren
