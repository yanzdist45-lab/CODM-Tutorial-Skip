#include <jni.h>
#include <android/log.h>
#include <unistd.h>
#include <pthread.h>
#include <sys/mman.h>

#include <cstdio>
#include <cstdint>
#include <cstring>

#include "zygisk.hpp"

#define LOG_TAG "CODM-TutorialSkip"

#define LOGI(...) \
    __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)

#define LOGE(...) \
    __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

/*
 * =========================================================
 * TARGET
 * =========================================================
 */

static constexpr const char *TARGET_PROCESS =
    "com.garena.game.codm";

static constexpr const char *TARGET_LIBRARY =
    "libunity.so";

/*
 * =========================================================
 * RVA
 *
 * Tutorial checker lama
 * =========================================================
 */

static constexpr uintptr_t RVA_GET_CURRENT_TUTORIAL =
    0x9DE143C;

static constexpr uintptr_t RVA_RESET_CURRENT_TUTORIAL =
    0x9DE1494;

static constexpr uintptr_t RVA_IS_FINISHED_INT =
    0x9DE3174;

static constexpr uintptr_t RVA_IS_FINISHED_TYPE =
    0x9DE31D4;

/*
 * =========================================================
 * RVA BARU DARI dump.cs
 *
 * public static void SkipAllTutorials(
 *     int reason = 0,
 *     bool showTip = true
 * )
 * =========================================================
 */

static constexpr uintptr_t RVA_SKIP_ALL_TUTORIALS =
    0xB69813C;

/*
 * =========================================================
 * ARM64
 * =========================================================
 */

static constexpr uint32_t ARM64_MOV_W0_0 =
    0x52800000;

static constexpr uint32_t ARM64_MOV_W0_1 =
    0x52800020;

static constexpr uint32_t ARM64_RET =
    0xD65F03C0;

/*
 * =========================================================
 * CONFIG
 * =========================================================
 */

static constexpr bool ENABLE_CHECKER_PATCHES = false;

static constexpr bool ENABLE_SKIP_ALL_CALL = true;

/*
 * Tunggu sebelum panggil SkipAllTutorials.
 *
 * Jangan terlalu cepat karena manager tutorial/game state
 * mungkin belum dibuat.
 */
static constexpr int SKIP_CALL_DELAY_SECONDS = 45;


/*
 * =========================================================
 * LIBRARY FINDER
 * =========================================================
 */

static uintptr_t find_library_base(const char *library) {

    FILE *fp = fopen("/proc/self/maps", "r");

    if (!fp) {
        return 0;
    }

    char line[1024];

    uintptr_t base = 0;

    while (fgets(line, sizeof(line), fp)) {

        if (!strstr(line, library)) {
            continue;
        }

        uintptr_t start = 0;
        uintptr_t end = 0;

        unsigned long offset = 0;

        char perms[8] = {};

        int parsed = sscanf(
            line,
            "%lx-%lx %7s %lx",
            &start,
            &end,
            perms,
            &offset
        );

        if (parsed < 4) {
            continue;
        }

        /*
         * Mapping offset 0 biasanya base ELF paling aman.
         */
        if (offset == 0) {

            base = start;

            break;
        }

        if (!base || start < base) {
            base = start;
        }
    }

    fclose(fp);

    return base;
}


/*
 * =========================================================
 * WRITE ARM64 INSTRUCTION
 * =========================================================
 */

static bool patch32(
    uintptr_t address,
    uint32_t value
) {

    long pageSize =
        sysconf(_SC_PAGESIZE);

    if (pageSize <= 0) {

        LOGE("invalid page size");

        return false;
    }

    uintptr_t page =
        address &
        ~(static_cast<uintptr_t>(pageSize) - 1);

    if (
        mprotect(
            reinterpret_cast<void *>(page),
            static_cast<size_t>(pageSize),
            PROT_READ |
            PROT_WRITE |
            PROT_EXEC
        ) != 0
    ) {

        LOGE(
            "mprotect failed @ %p",
            reinterpret_cast<void *>(address)
        );

        return false;
    }

    *reinterpret_cast<volatile uint32_t *>(address) =
        value;

    __builtin___clear_cache(
        reinterpret_cast<char *>(address),
        reinterpret_cast<char *>(address + 4)
    );

    /*
     * Balikin RX.
     */
    mprotect(
        reinterpret_cast<void *>(page),
        static_cast<size_t>(pageSize),
        PROT_READ |
        PROT_EXEC
    );

    return true;
}


/*
 * =========================================================
 * PATCH RETURN BOOL
 * =========================================================
 */

static bool patch_return_bool(
    uintptr_t base,
    uintptr_t rva,
    bool result
) {

    uintptr_t address =
        base + rva;

    uint32_t mov =
        result
        ? ARM64_MOV_W0_1
        : ARM64_MOV_W0_0;

    if (!patch32(address, mov)) {
        return false;
    }

    if (!patch32(
            address + 4,
            ARM64_RET
        )) {

        return false;
    }

    return true;
}


/*
 * =========================================================
 * PATCH VOID -> RET
 * =========================================================
 */

static bool patch_return_void(
    uintptr_t base,
    uintptr_t rva
) {

    return patch32(
        base + rva,
        ARM64_RET
    );
}


/*
 * =========================================================
 * PATCH CHECKER
 * =========================================================
 */

static void apply_checker_patches(
    uintptr_t base
) {

    LOGI(
        "apply checker patches base=%p",
        reinterpret_cast<void *>(base)
    );

    bool a =
        patch_return_bool(
            base,
            RVA_IS_FINISHED_INT,
            true
        );

    bool b =
        patch_return_bool(
            base,
            RVA_IS_FINISHED_TYPE,
            true
        );

    /*
     * Current tutorial -> 0
     */
    bool c =
        patch_return_bool(
            base,
            RVA_GET_CURRENT_TUTORIAL,
            false
        );

    /*
     * Cegah reset tutorial.
     */
    bool d =
        patch_return_void(
            base,
            RVA_RESET_CURRENT_TUTORIAL
        );

    LOGI(
        "checker patches: %d %d %d %d",
        a,
        b,
        c,
        d
    );
}


/*
 * =========================================================
 * SkipAllTutorials
 *
 * dump.cs:
 *
 * public static void SkipAllTutorials(
 *     int reason = 0,
 *     bool showTip = true
 * )
 *
 * Native IL2CPP biasanya punya argumen MethodInfo*
 * tersembunyi sebagai argumen terakhir.
 *
 * Jadi signature:
 *
 * void (
 *     int32_t reason,
 *     bool showTip,
 *     void *methodInfo
 * )
 * =========================================================
 */

using SkipAllTutorialsFn =
    void (*)(
        int32_t reason,
        bool showTip,
        void *methodInfo
    );


static bool call_skip_all_tutorials(
    uintptr_t base
) {

    uintptr_t address =
        base + RVA_SKIP_ALL_TUTORIALS;

    LOGI(
        "SkipAllTutorials addr=%p",
        reinterpret_cast<void *>(address)
    );

    auto fn =
        reinterpret_cast<SkipAllTutorialsFn>(
            address
        );

    if (!fn) {

        LOGE(
            "SkipAllTutorials function null"
        );

        return false;
    }

    /*
     * reason = 0
     * showTip = false
     * MethodInfo = nullptr
     */
    fn(
        0,
        false,
        nullptr
    );

    LOGI(
        "SkipAllTutorials called"
    );

    return true;
}


/*
 * =========================================================
 * WORKER
 * =========================================================
 */

static void *worker(void *) {

    LOGI("worker started");

    uintptr_t base = 0;

    /*
     * Tunggu libunity.so sampai muncul.
     *
     * 300 * 200ms = 60 detik.
     */
    for (
        int i = 0;
        i < 300;
        ++i
    ) {

        base =
            find_library_base(
                TARGET_LIBRARY
            );

        if (base) {
            break;
        }

        usleep(200000);
    }

    if (!base) {

        LOGE(
            "%s not found",
            TARGET_LIBRARY
        );

        return nullptr;
    }

    LOGI(
        "%s base=%p",
        TARGET_LIBRARY,
        reinterpret_cast<void *>(base)
    );


    /*
     * =====================================================
     * Checker patches
     * =====================================================
     */

    if (ENABLE_CHECKER_PATCHES) {

        /*
         * Kasih Unity waktu init sebentar.
         */
        sleep(2);

        /*
         * Ulang beberapa kali kalau ada init ulang.
         */
        for (
            int i = 0;
            i < 10;
            ++i
        ) {

            uintptr_t current =
                find_library_base(
                    TARGET_LIBRARY
                );

            if (current) {

                apply_checker_patches(
                    current
                );
            }

            sleep(1);
        }
    }


    /*
     * =====================================================
     * Tunggu game state/login manager siap.
     * =====================================================
     */

    if (ENABLE_SKIP_ALL_CALL) {

        LOGI(
            "waiting %d sec before SkipAllTutorials",
            SKIP_CALL_DELAY_SECONDS
        );

        sleep(
            SKIP_CALL_DELAY_SECONDS
        );

        uintptr_t current =
            find_library_base(
                TARGET_LIBRARY
            );

        if (!current) {

            LOGE(
                "library missing before SkipAllTutorials"
            );

            return nullptr;
        }

        call_skip_all_tutorials(
            current
        );
    }


    /*
     * =====================================================
     * Setelah call asli, pasang checker sekali lagi.
     * =====================================================
     */

    if (ENABLE_CHECKER_PATCHES) {

        sleep(2);

        uintptr_t current =
            find_library_base(
                TARGET_LIBRARY
            );

        if (current) {

            apply_checker_patches(
                current
            );
        }
    }

    LOGI(
        "worker finished"
    );

    return nullptr;
}


/*
 * =========================================================
 * ZYGISK MODULE
 * =========================================================
 */

class CodmTutorialSkip
    : public zygisk::ModuleBase {

public:

    void onLoad(
        zygisk::Api *api,
        JNIEnv *env
    ) override {

        api_ = api;
        env_ = env;
    }


    void preAppSpecialize(
        zygisk::AppSpecializeArgs *args
    ) override {

        target_ = false;

        if (
            !env_ ||
            !args ||
            !args->nice_name
        ) {

            return;
        }

        const char *name =
            env_->GetStringUTFChars(
                args->nice_name,
                nullptr
            );

        if (!name) {
            return;
        }

        if (
            strcmp(
                name,
                TARGET_PROCESS
            ) == 0
        ) {

            target_ = true;
        }

        env_->ReleaseStringUTFChars(
            args->nice_name,
            name
        );

        if (!target_) {

            /*
             * Jangan keep module di proses lain.
             */
            api_->setOption(
                zygisk::Option::
                    DLCLOSE_MODULE_LIBRARY
            );
        }
    }


    void postAppSpecialize(
        const zygisk::AppSpecializeArgs *
    ) override {

        if (!target_) {
            return;
        }

        LOGI(
            "target detected: %s",
            TARGET_PROCESS
        );

        pthread_t thread;

        int result =
            pthread_create(
                &thread,
                nullptr,
                worker,
                nullptr
            );

        if (result != 0) {

            LOGE(
                "pthread_create failed: %d",
                result
            );

            return;
        }

        pthread_detach(
            thread
        );
    }


private:

    zygisk::Api *api_ =
        nullptr;

    JNIEnv *env_ =
        nullptr;

    bool target_ =
        false;
};


REGISTER_ZYGISK_MODULE(
    CodmTutorialSkip
)
