// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Port of Ryujinx.Audio/Integration/IWritableEvent.cs (Ryubing fork, MIT): the system event an audio renderer session signals after
// each update. The C# interface is kept as an abstract class; the event itself is owned by the caller of the renderer.
#pragma once

namespace nxrt::audren {

class IWritableEvent {
  public:
    virtual ~IWritableEvent() = default;

    // Signal the event.
    virtual void Signal() = 0;

    // Clear the signaled state of the event.
    virtual void Clear() = 0;
};

}  // namespace nxrt::audren
