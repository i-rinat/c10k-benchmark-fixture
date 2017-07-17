#pragma once

#define RETRY_ON_EINTR(x)                                                                          \
    ({                                                                                             \
        typeof(x) ___tmp_res;                                                                      \
        do {                                                                                       \
            ___tmp_res = (x);                                                                      \
        } while (___tmp_res == -1 && errno == EINTR);                                              \
        ___tmp_res;                                                                                \
    })
