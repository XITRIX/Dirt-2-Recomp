// Native OpenAL/newlib and Mesa's bundled C11 shim use different enum values.
// Exercise both linked implementations before starting threads or GPU work.
#include <threads.h>
#include <pthread.h>
#include "recomp/host.h"
extern "C" int dirt2_mesa_tss_create(pthread_key_t*, void (*)(void*));
extern "C" int dirt2_mesa_tss_set(pthread_key_t, void*);
extern "C" void* dirt2_mesa_tss_get(pthread_key_t);
extern "C" void dirt2_mesa_tss_delete(pthread_key_t);

namespace switch_platform {
bool check_c11_abis() {
    tss_t native;
    pthread_key_t mesa;
    int value = 7;
    int rc = tss_create(&native, nullptr);
    rt::log("C11 ABI check: native tss_create=%d (expected %d)", rc, thrd_success);
    if (rc != thrd_success) return false;
    const bool native_ok = tss_set(native, &value) == thrd_success && tss_get(native) == &value;
    tss_delete(native);
    rc = dirt2_mesa_tss_create(&mesa, nullptr);
    rt::log("C11 ABI check: Mesa tss_create=%d (expected 0)", rc);
    if (rc != 0) return false;
    const bool mesa_ok = dirt2_mesa_tss_set(mesa, &value) == 0 && dirt2_mesa_tss_get(mesa) == &value;
    dirt2_mesa_tss_delete(mesa);
    mtx_t mutex;
    rc = mtx_init(&mutex, mtx_plain);
    if (rc != thrd_success) return false;
    const bool mutex_ok = mtx_lock(&mutex) == thrd_success && mtx_unlock(&mutex) == thrd_success;
    mtx_destroy(&mutex);
    cnd_t cond;
    rc = cnd_init(&cond);
    if (rc != thrd_success) return false;
    const bool cond_ok = cnd_signal(&cond) == thrd_success;
    cnd_destroy(&cond);
    thrd_t worker;
    rc = thrd_create(&worker, [](void*) -> int { return 37; }, nullptr);
    if (rc != thrd_success) return false;
    int result = 0;
    const bool thread_ok = thrd_join(worker, &result) == thrd_success && result == 37;
    const bool ok = native_ok && mesa_ok && mutex_ok && cond_ok && thread_ok;
    rt::log("C11 ABI check: TLS/mutex/condition/thread %s", ok ? "PASS" : "FAIL");
    return ok;
}
}
