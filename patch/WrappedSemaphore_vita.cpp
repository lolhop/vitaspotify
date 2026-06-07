#include "WrappedSemaphore.h"

using namespace bell;

WrappedSemaphore::WrappedSemaphore(int count) {
    (void)count;
    semaphoreHandle = sceKernelCreateSema("bell_sem", 0, 0, 64, nullptr);
}

WrappedSemaphore::~WrappedSemaphore() {
    if (semaphoreHandle >= 0) {
        sceKernelDeleteSema(semaphoreHandle);
        semaphoreHandle = -1;
    }
}

int WrappedSemaphore::wait() {
    return sceKernelWaitSema(semaphoreHandle, 1, nullptr);
}

int WrappedSemaphore::twait(long milliseconds) {
    SceUInt timeout = static_cast<SceUInt>(milliseconds * 1000);
    return sceKernelWaitSema(semaphoreHandle, 1, &timeout);
}

void WrappedSemaphore::give() {
    sceKernelSignalSema(semaphoreHandle, 1);
}
