// SPDX-FileCopyrightText: Copyright 2024-2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include "common/assert.h"
#include "common/types.h"
#include "core/libraries/kernel/equeue.h"
#include "core/libraries/kernel/orbis_error.h"
#include "core/platform.h"

namespace Libraries::GnmDriver {

inline bool IsValidEqEventType(Platform::InterruptId id) {
    return (static_cast<u32>(id) >= static_cast<u32>(Platform::InterruptId::Compute0RelMem) &&
            static_cast<u32>(id) <= static_cast<u32>(Platform::InterruptId::Compute6RelMem)) ||
           static_cast<u32>(id) == static_cast<u32>(Platform::InterruptId::GfxEop);
}

inline s32 RegisterGnmEqEvent(Kernel::OrbisKernelEqueue eq, u64 id, void* udata) {
    auto equeue = Kernel::AcquireEqueue(eq);
    if (!equeue) {
        return ORBIS_KERNEL_ERROR_EBADF;
    }

    Kernel::EqueueEvent kernel_event{};
    kernel_event.event.ident = id;
    kernel_event.event.filter = Kernel::OrbisKernelEvent::Filter::GraphicsCore;
    kernel_event.event.flags = Kernel::OrbisKernelEvent::Flags::Add;
    kernel_event.event.fflags = 0;
    kernel_event.event.data = id;
    kernel_event.event.udata = udata;

    if (!equeue->AddEvent(kernel_event)) {
        return ORBIS_KERNEL_ERROR_EBADF;
    }

    auto* const uid = reinterpret_cast<void*>(static_cast<uintptr_t>(eq));
    Platform::IrqC::Instance()->RegisterManaged(
        static_cast<Platform::InterruptId>(id),
        [eq, id](Platform::InterruptId irq) -> bool {
            ASSERT_MSG(irq == static_cast<Platform::InterruptId>(id), "An unexpected IRQ occured");

            if (!IsValidEqEventType(irq)) {
                return true;
            }

            const auto target = Kernel::AcquireEqueue(eq);
            if (!target) {
                return false;
            }

            target->TriggerEvent(id, Kernel::OrbisKernelEvent::Filter::GraphicsCore,
                                 reinterpret_cast<void*>(id));
            return !target->IsClosed();
        },
        uid);
    return ORBIS_OK;
}

inline s32 UnregisterGnmEqEvent(Kernel::OrbisKernelEqueue eq, u64 id) {
    auto* const uid = reinterpret_cast<void*>(static_cast<uintptr_t>(eq));
    auto equeue = Kernel::AcquireEqueue(eq);
    if (!equeue) {
        Platform::IrqC::Instance()->Unregister(static_cast<Platform::InterruptId>(id), uid);
        return ORBIS_KERNEL_ERROR_EBADF;
    }

    equeue->RemoveEvent(id, Kernel::OrbisKernelEvent::Filter::GraphicsCore);
    Platform::IrqC::Instance()->Unregister(static_cast<Platform::InterruptId>(id), uid);
    return ORBIS_OK;
}

} // namespace Libraries::GnmDriver
