#pragma once

#include <cstddef>

namespace gdl {
/** Decompose the actual stored record, not a parallel list of selected members.
 * Adding/removing a member makes the selected structured binding ill-formed.
 * This is visitation only: no byte copying, reflection of padding, or restore.
 * Private records use a friend visitor that binds their members in their owner. */
template <std::size_t Count, class T, class Visitor>
// NOLINTNEXTLINE(cppcoreguidelines-missing-std-forward): the visitor is invoked here, not passed on
void visitNativeFields(const T& state, Visitor&& visitor) {
    static_assert(Count >= 1 && Count <= 64, "Add an explicit decomposition for this record");
    if constexpr (Count == 1) {
        const auto& [v0] = state;
        visitor(v0);
    } else if constexpr (Count == 2) {
        const auto& [v0, v1] = state;
        visitor(v0, v1);
    } else if constexpr (Count == 3) {
        const auto& [v0, v1, v2] = state;
        visitor(v0, v1, v2);
    } else if constexpr (Count == 4) {
        const auto& [v0, v1, v2, v3] = state;
        visitor(v0, v1, v2, v3);
    } else if constexpr (Count == 5) {
        const auto& [v0, v1, v2, v3, v4] = state;
        visitor(v0, v1, v2, v3, v4);
    } else if constexpr (Count == 6) {
        const auto& [v0, v1, v2, v3, v4, v5] = state;
        visitor(v0, v1, v2, v3, v4, v5);
    } else if constexpr (Count == 7) {
        const auto& [v0, v1, v2, v3, v4, v5, v6] = state;
        visitor(v0, v1, v2, v3, v4, v5, v6);
    } else if constexpr (Count == 8) {
        const auto& [v0, v1, v2, v3, v4, v5, v6, v7] = state;
        visitor(v0, v1, v2, v3, v4, v5, v6, v7);
    } else if constexpr (Count == 9) {
        const auto& [v0, v1, v2, v3, v4, v5, v6, v7, v8] = state;
        visitor(v0, v1, v2, v3, v4, v5, v6, v7, v8);
    } else if constexpr (Count == 10) {
        const auto& [v0, v1, v2, v3, v4, v5, v6, v7, v8, v9] = state;
        visitor(v0, v1, v2, v3, v4, v5, v6, v7, v8, v9);
    } else if constexpr (Count == 11) {
        const auto& [v0, v1, v2, v3, v4, v5, v6, v7, v8, v9, v10] = state;
        visitor(v0, v1, v2, v3, v4, v5, v6, v7, v8, v9, v10);
    } else if constexpr (Count == 12) {
        const auto& [v0, v1, v2, v3, v4, v5, v6, v7, v8, v9, v10, v11] = state;
        visitor(v0, v1, v2, v3, v4, v5, v6, v7, v8, v9, v10, v11);
    } else if constexpr (Count == 13) {
        const auto& [v0, v1, v2, v3, v4, v5, v6, v7, v8, v9, v10, v11, v12] = state;
        visitor(v0, v1, v2, v3, v4, v5, v6, v7, v8, v9, v10, v11, v12);
    } else if constexpr (Count == 14) {
        const auto& [v0, v1, v2, v3, v4, v5, v6, v7, v8, v9, v10, v11, v12, v13] = state;
        visitor(v0, v1, v2, v3, v4, v5, v6, v7, v8, v9, v10, v11, v12, v13);
    } else if constexpr (Count == 15) {
        const auto& [v0, v1, v2, v3, v4, v5, v6, v7, v8, v9, v10, v11, v12, v13, v14] = state;
        visitor(v0, v1, v2, v3, v4, v5, v6, v7, v8, v9, v10, v11, v12, v13, v14);
    } else if constexpr (Count == 16) {
        const auto& [v0, v1, v2, v3, v4, v5, v6, v7, v8, v9, v10, v11, v12, v13, v14, v15] = state;
        visitor(v0, v1, v2, v3, v4, v5, v6, v7, v8, v9, v10, v11, v12, v13, v14, v15);
    } else if constexpr (Count == 17) {
        const auto& [v0, v1, v2, v3, v4, v5, v6, v7, v8, v9, v10, v11, v12, v13, v14, v15, v16] =
            state;
        visitor(v0, v1, v2, v3, v4, v5, v6, v7, v8, v9, v10, v11, v12, v13, v14, v15, v16);
    } else if constexpr (Count == 18) {
        const auto& [v0, v1, v2, v3, v4, v5, v6, v7, v8, v9, v10, v11, v12, v13, v14, v15, v16,
                     v17] = state;
        visitor(v0, v1, v2, v3, v4, v5, v6, v7, v8, v9, v10, v11, v12, v13, v14, v15, v16, v17);
    } else if constexpr (Count == 19) {
        const auto& [v0, v1, v2, v3, v4, v5, v6, v7, v8, v9, v10, v11, v12, v13, v14, v15, v16, v17,
                     v18] = state;
        visitor(v0, v1, v2, v3, v4, v5, v6, v7, v8, v9, v10, v11, v12, v13, v14, v15, v16, v17,
                v18);
    } else if constexpr (Count == 20) {
        const auto& [v0, v1, v2, v3, v4, v5, v6, v7, v8, v9, v10, v11, v12, v13, v14, v15, v16, v17,
                     v18, v19] = state;
        visitor(v0, v1, v2, v3, v4, v5, v6, v7, v8, v9, v10, v11, v12, v13, v14, v15, v16, v17, v18,
                v19);
    } else if constexpr (Count == 21) {
        const auto& [v0, v1, v2, v3, v4, v5, v6, v7, v8, v9, v10, v11, v12, v13, v14, v15, v16, v17,
                     v18, v19, v20] = state;
        visitor(v0, v1, v2, v3, v4, v5, v6, v7, v8, v9, v10, v11, v12, v13, v14, v15, v16, v17, v18,
                v19, v20);
    } else if constexpr (Count == 22) {
        const auto& [v0, v1, v2, v3, v4, v5, v6, v7, v8, v9, v10, v11, v12, v13, v14, v15, v16, v17,
                     v18, v19, v20, v21] = state;
        visitor(v0, v1, v2, v3, v4, v5, v6, v7, v8, v9, v10, v11, v12, v13, v14, v15, v16, v17, v18,
                v19, v20, v21);
    } else if constexpr (Count == 23) {
        const auto& [v0, v1, v2, v3, v4, v5, v6, v7, v8, v9, v10, v11, v12, v13, v14, v15, v16, v17,
                     v18, v19, v20, v21, v22] = state;
        visitor(v0, v1, v2, v3, v4, v5, v6, v7, v8, v9, v10, v11, v12, v13, v14, v15, v16, v17, v18,
                v19, v20, v21, v22);
    } else if constexpr (Count == 24) {
        const auto& [v0, v1, v2, v3, v4, v5, v6, v7, v8, v9, v10, v11, v12, v13, v14, v15, v16, v17,
                     v18, v19, v20, v21, v22, v23] = state;
        visitor(v0, v1, v2, v3, v4, v5, v6, v7, v8, v9, v10, v11, v12, v13, v14, v15, v16, v17, v18,
                v19, v20, v21, v22, v23);
    } else if constexpr (Count == 25) {
        const auto& [v0, v1, v2, v3, v4, v5, v6, v7, v8, v9, v10, v11, v12, v13, v14, v15, v16, v17,
                     v18, v19, v20, v21, v22, v23, v24] = state;
        visitor(v0, v1, v2, v3, v4, v5, v6, v7, v8, v9, v10, v11, v12, v13, v14, v15, v16, v17, v18,
                v19, v20, v21, v22, v23, v24);
    } else if constexpr (Count == 26) {
        const auto& [v0, v1, v2, v3, v4, v5, v6, v7, v8, v9, v10, v11, v12, v13, v14, v15, v16, v17,
                     v18, v19, v20, v21, v22, v23, v24, v25] = state;
        visitor(v0, v1, v2, v3, v4, v5, v6, v7, v8, v9, v10, v11, v12, v13, v14, v15, v16, v17, v18,
                v19, v20, v21, v22, v23, v24, v25);
    } else if constexpr (Count == 27) {
        const auto& [v0, v1, v2, v3, v4, v5, v6, v7, v8, v9, v10, v11, v12, v13, v14, v15, v16, v17,
                     v18, v19, v20, v21, v22, v23, v24, v25, v26] = state;
        visitor(v0, v1, v2, v3, v4, v5, v6, v7, v8, v9, v10, v11, v12, v13, v14, v15, v16, v17, v18,
                v19, v20, v21, v22, v23, v24, v25, v26);
    } else if constexpr (Count == 28) {
        const auto& [v0, v1, v2, v3, v4, v5, v6, v7, v8, v9, v10, v11, v12, v13, v14, v15, v16, v17,
                     v18, v19, v20, v21, v22, v23, v24, v25, v26, v27] = state;
        visitor(v0, v1, v2, v3, v4, v5, v6, v7, v8, v9, v10, v11, v12, v13, v14, v15, v16, v17, v18,
                v19, v20, v21, v22, v23, v24, v25, v26, v27);
    } else if constexpr (Count == 29) {
        const auto& [v0, v1, v2, v3, v4, v5, v6, v7, v8, v9, v10, v11, v12, v13, v14, v15, v16, v17,
                     v18, v19, v20, v21, v22, v23, v24, v25, v26, v27, v28] = state;
        visitor(v0, v1, v2, v3, v4, v5, v6, v7, v8, v9, v10, v11, v12, v13, v14, v15, v16, v17, v18,
                v19, v20, v21, v22, v23, v24, v25, v26, v27, v28);
    } else if constexpr (Count == 30) {
        const auto& [v0, v1, v2, v3, v4, v5, v6, v7, v8, v9, v10, v11, v12, v13, v14, v15, v16, v17,
                     v18, v19, v20, v21, v22, v23, v24, v25, v26, v27, v28, v29] = state;
        visitor(v0, v1, v2, v3, v4, v5, v6, v7, v8, v9, v10, v11, v12, v13, v14, v15, v16, v17, v18,
                v19, v20, v21, v22, v23, v24, v25, v26, v27, v28, v29);
    } else if constexpr (Count == 31) {
        const auto& [v0, v1, v2, v3, v4, v5, v6, v7, v8, v9, v10, v11, v12, v13, v14, v15, v16, v17,
                     v18, v19, v20, v21, v22, v23, v24, v25, v26, v27, v28, v29, v30] = state;
        visitor(v0, v1, v2, v3, v4, v5, v6, v7, v8, v9, v10, v11, v12, v13, v14, v15, v16, v17, v18,
                v19, v20, v21, v22, v23, v24, v25, v26, v27, v28, v29, v30);
    } else if constexpr (Count == 32) {
        const auto& [v0, v1, v2, v3, v4, v5, v6, v7, v8, v9, v10, v11, v12, v13, v14, v15, v16, v17,
                     v18, v19, v20, v21, v22, v23, v24, v25, v26, v27, v28, v29, v30, v31] = state;
        visitor(v0, v1, v2, v3, v4, v5, v6, v7, v8, v9, v10, v11, v12, v13, v14, v15, v16, v17, v18,
                v19, v20, v21, v22, v23, v24, v25, v26, v27, v28, v29, v30, v31);
    } else if constexpr (Count == 33) {
        const auto& [v0, v1, v2, v3, v4, v5, v6, v7, v8, v9, v10, v11, v12, v13, v14, v15, v16, v17,
                     v18, v19, v20, v21, v22, v23, v24, v25, v26, v27, v28, v29, v30, v31, v32] =
            state;
        visitor(v0, v1, v2, v3, v4, v5, v6, v7, v8, v9, v10, v11, v12, v13, v14, v15, v16, v17, v18,
                v19, v20, v21, v22, v23, v24, v25, v26, v27, v28, v29, v30, v31, v32);
    } else if constexpr (Count == 34) {
        const auto& [v0, v1, v2, v3, v4, v5, v6, v7, v8, v9, v10, v11, v12, v13, v14, v15, v16, v17,
                     v18, v19, v20, v21, v22, v23, v24, v25, v26, v27, v28, v29, v30, v31, v32,
                     v33] = state;
        visitor(v0, v1, v2, v3, v4, v5, v6, v7, v8, v9, v10, v11, v12, v13, v14, v15, v16, v17, v18,
                v19, v20, v21, v22, v23, v24, v25, v26, v27, v28, v29, v30, v31, v32, v33);
    } else if constexpr (Count == 35) {
        const auto& [v0, v1, v2, v3, v4, v5, v6, v7, v8, v9, v10, v11, v12, v13, v14, v15, v16, v17,
                     v18, v19, v20, v21, v22, v23, v24, v25, v26, v27, v28, v29, v30, v31, v32, v33,
                     v34] = state;
        visitor(v0, v1, v2, v3, v4, v5, v6, v7, v8, v9, v10, v11, v12, v13, v14, v15, v16, v17, v18,
                v19, v20, v21, v22, v23, v24, v25, v26, v27, v28, v29, v30, v31, v32, v33, v34);
    } else if constexpr (Count == 36) {
        const auto& [v0, v1, v2, v3, v4, v5, v6, v7, v8, v9, v10, v11, v12, v13, v14, v15, v16, v17,
                     v18, v19, v20, v21, v22, v23, v24, v25, v26, v27, v28, v29, v30, v31, v32, v33,
                     v34, v35] = state;
        visitor(v0, v1, v2, v3, v4, v5, v6, v7, v8, v9, v10, v11, v12, v13, v14, v15, v16, v17, v18,
                v19, v20, v21, v22, v23, v24, v25, v26, v27, v28, v29, v30, v31, v32, v33, v34,
                v35);
    } else if constexpr (Count == 37) {
        const auto& [v0, v1, v2, v3, v4, v5, v6, v7, v8, v9, v10, v11, v12, v13, v14, v15, v16, v17,
                     v18, v19, v20, v21, v22, v23, v24, v25, v26, v27, v28, v29, v30, v31, v32, v33,
                     v34, v35, v36] = state;
        visitor(v0, v1, v2, v3, v4, v5, v6, v7, v8, v9, v10, v11, v12, v13, v14, v15, v16, v17, v18,
                v19, v20, v21, v22, v23, v24, v25, v26, v27, v28, v29, v30, v31, v32, v33, v34, v35,
                v36);
    } else if constexpr (Count == 38) {
        const auto& [v0, v1, v2, v3, v4, v5, v6, v7, v8, v9, v10, v11, v12, v13, v14, v15, v16, v17,
                     v18, v19, v20, v21, v22, v23, v24, v25, v26, v27, v28, v29, v30, v31, v32, v33,
                     v34, v35, v36, v37] = state;
        visitor(v0, v1, v2, v3, v4, v5, v6, v7, v8, v9, v10, v11, v12, v13, v14, v15, v16, v17, v18,
                v19, v20, v21, v22, v23, v24, v25, v26, v27, v28, v29, v30, v31, v32, v33, v34, v35,
                v36, v37);
    } else if constexpr (Count == 39) {
        const auto& [v0, v1, v2, v3, v4, v5, v6, v7, v8, v9, v10, v11, v12, v13, v14, v15, v16, v17,
                     v18, v19, v20, v21, v22, v23, v24, v25, v26, v27, v28, v29, v30, v31, v32, v33,
                     v34, v35, v36, v37, v38] = state;
        visitor(v0, v1, v2, v3, v4, v5, v6, v7, v8, v9, v10, v11, v12, v13, v14, v15, v16, v17, v18,
                v19, v20, v21, v22, v23, v24, v25, v26, v27, v28, v29, v30, v31, v32, v33, v34, v35,
                v36, v37, v38);
    } else if constexpr (Count == 40) {
        const auto& [v0, v1, v2, v3, v4, v5, v6, v7, v8, v9, v10, v11, v12, v13, v14, v15, v16, v17,
                     v18, v19, v20, v21, v22, v23, v24, v25, v26, v27, v28, v29, v30, v31, v32, v33,
                     v34, v35, v36, v37, v38, v39] = state;
        visitor(v0, v1, v2, v3, v4, v5, v6, v7, v8, v9, v10, v11, v12, v13, v14, v15, v16, v17, v18,
                v19, v20, v21, v22, v23, v24, v25, v26, v27, v28, v29, v30, v31, v32, v33, v34, v35,
                v36, v37, v38, v39);
    } else if constexpr (Count == 41) {
        const auto& [v0, v1, v2, v3, v4, v5, v6, v7, v8, v9, v10, v11, v12, v13, v14, v15, v16, v17,
                     v18, v19, v20, v21, v22, v23, v24, v25, v26, v27, v28, v29, v30, v31, v32, v33,
                     v34, v35, v36, v37, v38, v39, v40] = state;
        visitor(v0, v1, v2, v3, v4, v5, v6, v7, v8, v9, v10, v11, v12, v13, v14, v15, v16, v17, v18,
                v19, v20, v21, v22, v23, v24, v25, v26, v27, v28, v29, v30, v31, v32, v33, v34, v35,
                v36, v37, v38, v39, v40);
    } else if constexpr (Count == 42) {
        const auto& [v0, v1, v2, v3, v4, v5, v6, v7, v8, v9, v10, v11, v12, v13, v14, v15, v16, v17,
                     v18, v19, v20, v21, v22, v23, v24, v25, v26, v27, v28, v29, v30, v31, v32, v33,
                     v34, v35, v36, v37, v38, v39, v40, v41] = state;
        visitor(v0, v1, v2, v3, v4, v5, v6, v7, v8, v9, v10, v11, v12, v13, v14, v15, v16, v17, v18,
                v19, v20, v21, v22, v23, v24, v25, v26, v27, v28, v29, v30, v31, v32, v33, v34, v35,
                v36, v37, v38, v39, v40, v41);
    } else if constexpr (Count == 43) {
        const auto& [v0, v1, v2, v3, v4, v5, v6, v7, v8, v9, v10, v11, v12, v13, v14, v15, v16, v17,
                     v18, v19, v20, v21, v22, v23, v24, v25, v26, v27, v28, v29, v30, v31, v32, v33,
                     v34, v35, v36, v37, v38, v39, v40, v41, v42] = state;
        visitor(v0, v1, v2, v3, v4, v5, v6, v7, v8, v9, v10, v11, v12, v13, v14, v15, v16, v17, v18,
                v19, v20, v21, v22, v23, v24, v25, v26, v27, v28, v29, v30, v31, v32, v33, v34, v35,
                v36, v37, v38, v39, v40, v41, v42);
    } else if constexpr (Count == 44) {
        const auto& [v0, v1, v2, v3, v4, v5, v6, v7, v8, v9, v10, v11, v12, v13, v14, v15, v16, v17,
                     v18, v19, v20, v21, v22, v23, v24, v25, v26, v27, v28, v29, v30, v31, v32, v33,
                     v34, v35, v36, v37, v38, v39, v40, v41, v42, v43] = state;
        visitor(v0, v1, v2, v3, v4, v5, v6, v7, v8, v9, v10, v11, v12, v13, v14, v15, v16, v17, v18,
                v19, v20, v21, v22, v23, v24, v25, v26, v27, v28, v29, v30, v31, v32, v33, v34, v35,
                v36, v37, v38, v39, v40, v41, v42, v43);
    } else if constexpr (Count == 45) {
        const auto& [v0, v1, v2, v3, v4, v5, v6, v7, v8, v9, v10, v11, v12, v13, v14, v15, v16, v17,
                     v18, v19, v20, v21, v22, v23, v24, v25, v26, v27, v28, v29, v30, v31, v32, v33,
                     v34, v35, v36, v37, v38, v39, v40, v41, v42, v43, v44] = state;
        visitor(v0, v1, v2, v3, v4, v5, v6, v7, v8, v9, v10, v11, v12, v13, v14, v15, v16, v17, v18,
                v19, v20, v21, v22, v23, v24, v25, v26, v27, v28, v29, v30, v31, v32, v33, v34, v35,
                v36, v37, v38, v39, v40, v41, v42, v43, v44);
    } else if constexpr (Count == 46) {
        const auto& [v0, v1, v2, v3, v4, v5, v6, v7, v8, v9, v10, v11, v12, v13, v14, v15, v16, v17,
                     v18, v19, v20, v21, v22, v23, v24, v25, v26, v27, v28, v29, v30, v31, v32, v33,
                     v34, v35, v36, v37, v38, v39, v40, v41, v42, v43, v44, v45] = state;
        visitor(v0, v1, v2, v3, v4, v5, v6, v7, v8, v9, v10, v11, v12, v13, v14, v15, v16, v17, v18,
                v19, v20, v21, v22, v23, v24, v25, v26, v27, v28, v29, v30, v31, v32, v33, v34, v35,
                v36, v37, v38, v39, v40, v41, v42, v43, v44, v45);
    } else if constexpr (Count == 47) {
        const auto& [v0, v1, v2, v3, v4, v5, v6, v7, v8, v9, v10, v11, v12, v13, v14, v15, v16, v17,
                     v18, v19, v20, v21, v22, v23, v24, v25, v26, v27, v28, v29, v30, v31, v32, v33,
                     v34, v35, v36, v37, v38, v39, v40, v41, v42, v43, v44, v45, v46] = state;
        visitor(v0, v1, v2, v3, v4, v5, v6, v7, v8, v9, v10, v11, v12, v13, v14, v15, v16, v17, v18,
                v19, v20, v21, v22, v23, v24, v25, v26, v27, v28, v29, v30, v31, v32, v33, v34, v35,
                v36, v37, v38, v39, v40, v41, v42, v43, v44, v45, v46);
    } else if constexpr (Count == 48) {
        const auto& [v0, v1, v2, v3, v4, v5, v6, v7, v8, v9, v10, v11, v12, v13, v14, v15, v16, v17,
                     v18, v19, v20, v21, v22, v23, v24, v25, v26, v27, v28, v29, v30, v31, v32, v33,
                     v34, v35, v36, v37, v38, v39, v40, v41, v42, v43, v44, v45, v46, v47] = state;
        visitor(v0, v1, v2, v3, v4, v5, v6, v7, v8, v9, v10, v11, v12, v13, v14, v15, v16, v17, v18,
                v19, v20, v21, v22, v23, v24, v25, v26, v27, v28, v29, v30, v31, v32, v33, v34, v35,
                v36, v37, v38, v39, v40, v41, v42, v43, v44, v45, v46, v47);
    } else if constexpr (Count == 49) {
        const auto& [v0, v1, v2, v3, v4, v5, v6, v7, v8, v9, v10, v11, v12, v13, v14, v15, v16, v17,
                     v18, v19, v20, v21, v22, v23, v24, v25, v26, v27, v28, v29, v30, v31, v32, v33,
                     v34, v35, v36, v37, v38, v39, v40, v41, v42, v43, v44, v45, v46, v47, v48] =
            state;
        visitor(v0, v1, v2, v3, v4, v5, v6, v7, v8, v9, v10, v11, v12, v13, v14, v15, v16, v17, v18,
                v19, v20, v21, v22, v23, v24, v25, v26, v27, v28, v29, v30, v31, v32, v33, v34, v35,
                v36, v37, v38, v39, v40, v41, v42, v43, v44, v45, v46, v47, v48);
    } else if constexpr (Count == 50) {
        const auto& [v0, v1, v2, v3, v4, v5, v6, v7, v8, v9, v10, v11, v12, v13, v14, v15, v16, v17,
                     v18, v19, v20, v21, v22, v23, v24, v25, v26, v27, v28, v29, v30, v31, v32, v33,
                     v34, v35, v36, v37, v38, v39, v40, v41, v42, v43, v44, v45, v46, v47, v48,
                     v49] = state;
        visitor(v0, v1, v2, v3, v4, v5, v6, v7, v8, v9, v10, v11, v12, v13, v14, v15, v16, v17, v18,
                v19, v20, v21, v22, v23, v24, v25, v26, v27, v28, v29, v30, v31, v32, v33, v34, v35,
                v36, v37, v38, v39, v40, v41, v42, v43, v44, v45, v46, v47, v48, v49);
    } else if constexpr (Count == 51) {
        const auto& [v0, v1, v2, v3, v4, v5, v6, v7, v8, v9, v10, v11, v12, v13, v14, v15, v16, v17,
                     v18, v19, v20, v21, v22, v23, v24, v25, v26, v27, v28, v29, v30, v31, v32, v33,
                     v34, v35, v36, v37, v38, v39, v40, v41, v42, v43, v44, v45, v46, v47, v48, v49,
                     v50] = state;
        visitor(v0, v1, v2, v3, v4, v5, v6, v7, v8, v9, v10, v11, v12, v13, v14, v15, v16, v17, v18,
                v19, v20, v21, v22, v23, v24, v25, v26, v27, v28, v29, v30, v31, v32, v33, v34, v35,
                v36, v37, v38, v39, v40, v41, v42, v43, v44, v45, v46, v47, v48, v49, v50);
    } else if constexpr (Count == 52) {
        const auto& [v0, v1, v2, v3, v4, v5, v6, v7, v8, v9, v10, v11, v12, v13, v14, v15, v16, v17,
                     v18, v19, v20, v21, v22, v23, v24, v25, v26, v27, v28, v29, v30, v31, v32, v33,
                     v34, v35, v36, v37, v38, v39, v40, v41, v42, v43, v44, v45, v46, v47, v48, v49,
                     v50, v51] = state;
        visitor(v0, v1, v2, v3, v4, v5, v6, v7, v8, v9, v10, v11, v12, v13, v14, v15, v16, v17, v18,
                v19, v20, v21, v22, v23, v24, v25, v26, v27, v28, v29, v30, v31, v32, v33, v34, v35,
                v36, v37, v38, v39, v40, v41, v42, v43, v44, v45, v46, v47, v48, v49, v50, v51);
    } else if constexpr (Count == 53) {
        const auto& [v0, v1, v2, v3, v4, v5, v6, v7, v8, v9, v10, v11, v12, v13, v14, v15, v16, v17,
                     v18, v19, v20, v21, v22, v23, v24, v25, v26, v27, v28, v29, v30, v31, v32, v33,
                     v34, v35, v36, v37, v38, v39, v40, v41, v42, v43, v44, v45, v46, v47, v48, v49,
                     v50, v51, v52] = state;
        visitor(v0, v1, v2, v3, v4, v5, v6, v7, v8, v9, v10, v11, v12, v13, v14, v15, v16, v17, v18,
                v19, v20, v21, v22, v23, v24, v25, v26, v27, v28, v29, v30, v31, v32, v33, v34, v35,
                v36, v37, v38, v39, v40, v41, v42, v43, v44, v45, v46, v47, v48, v49, v50, v51,
                v52);
    } else if constexpr (Count == 54) {
        const auto& [v0, v1, v2, v3, v4, v5, v6, v7, v8, v9, v10, v11, v12, v13, v14, v15, v16, v17,
                     v18, v19, v20, v21, v22, v23, v24, v25, v26, v27, v28, v29, v30, v31, v32, v33,
                     v34, v35, v36, v37, v38, v39, v40, v41, v42, v43, v44, v45, v46, v47, v48, v49,
                     v50, v51, v52, v53] = state;
        visitor(v0, v1, v2, v3, v4, v5, v6, v7, v8, v9, v10, v11, v12, v13, v14, v15, v16, v17, v18,
                v19, v20, v21, v22, v23, v24, v25, v26, v27, v28, v29, v30, v31, v32, v33, v34, v35,
                v36, v37, v38, v39, v40, v41, v42, v43, v44, v45, v46, v47, v48, v49, v50, v51, v52,
                v53);
    } else if constexpr (Count == 55) {
        const auto& [v0, v1, v2, v3, v4, v5, v6, v7, v8, v9, v10, v11, v12, v13, v14, v15, v16, v17,
                     v18, v19, v20, v21, v22, v23, v24, v25, v26, v27, v28, v29, v30, v31, v32, v33,
                     v34, v35, v36, v37, v38, v39, v40, v41, v42, v43, v44, v45, v46, v47, v48, v49,
                     v50, v51, v52, v53, v54] = state;
        visitor(v0, v1, v2, v3, v4, v5, v6, v7, v8, v9, v10, v11, v12, v13, v14, v15, v16, v17, v18,
                v19, v20, v21, v22, v23, v24, v25, v26, v27, v28, v29, v30, v31, v32, v33, v34, v35,
                v36, v37, v38, v39, v40, v41, v42, v43, v44, v45, v46, v47, v48, v49, v50, v51, v52,
                v53, v54);
    } else if constexpr (Count == 56) {
        const auto& [v0, v1, v2, v3, v4, v5, v6, v7, v8, v9, v10, v11, v12, v13, v14, v15, v16, v17,
                     v18, v19, v20, v21, v22, v23, v24, v25, v26, v27, v28, v29, v30, v31, v32, v33,
                     v34, v35, v36, v37, v38, v39, v40, v41, v42, v43, v44, v45, v46, v47, v48, v49,
                     v50, v51, v52, v53, v54, v55] = state;
        visitor(v0, v1, v2, v3, v4, v5, v6, v7, v8, v9, v10, v11, v12, v13, v14, v15, v16, v17, v18,
                v19, v20, v21, v22, v23, v24, v25, v26, v27, v28, v29, v30, v31, v32, v33, v34, v35,
                v36, v37, v38, v39, v40, v41, v42, v43, v44, v45, v46, v47, v48, v49, v50, v51, v52,
                v53, v54, v55);
    } else if constexpr (Count == 57) {
        const auto& [v0, v1, v2, v3, v4, v5, v6, v7, v8, v9, v10, v11, v12, v13, v14, v15, v16, v17,
                     v18, v19, v20, v21, v22, v23, v24, v25, v26, v27, v28, v29, v30, v31, v32, v33,
                     v34, v35, v36, v37, v38, v39, v40, v41, v42, v43, v44, v45, v46, v47, v48, v49,
                     v50, v51, v52, v53, v54, v55, v56] = state;
        visitor(v0, v1, v2, v3, v4, v5, v6, v7, v8, v9, v10, v11, v12, v13, v14, v15, v16, v17, v18,
                v19, v20, v21, v22, v23, v24, v25, v26, v27, v28, v29, v30, v31, v32, v33, v34, v35,
                v36, v37, v38, v39, v40, v41, v42, v43, v44, v45, v46, v47, v48, v49, v50, v51, v52,
                v53, v54, v55, v56);
    } else if constexpr (Count == 58) {
        const auto& [v0, v1, v2, v3, v4, v5, v6, v7, v8, v9, v10, v11, v12, v13, v14, v15, v16, v17,
                     v18, v19, v20, v21, v22, v23, v24, v25, v26, v27, v28, v29, v30, v31, v32, v33,
                     v34, v35, v36, v37, v38, v39, v40, v41, v42, v43, v44, v45, v46, v47, v48, v49,
                     v50, v51, v52, v53, v54, v55, v56, v57] = state;
        visitor(v0, v1, v2, v3, v4, v5, v6, v7, v8, v9, v10, v11, v12, v13, v14, v15, v16, v17, v18,
                v19, v20, v21, v22, v23, v24, v25, v26, v27, v28, v29, v30, v31, v32, v33, v34, v35,
                v36, v37, v38, v39, v40, v41, v42, v43, v44, v45, v46, v47, v48, v49, v50, v51, v52,
                v53, v54, v55, v56, v57);
    } else if constexpr (Count == 59) {
        const auto& [v0, v1, v2, v3, v4, v5, v6, v7, v8, v9, v10, v11, v12, v13, v14, v15, v16, v17,
                     v18, v19, v20, v21, v22, v23, v24, v25, v26, v27, v28, v29, v30, v31, v32, v33,
                     v34, v35, v36, v37, v38, v39, v40, v41, v42, v43, v44, v45, v46, v47, v48, v49,
                     v50, v51, v52, v53, v54, v55, v56, v57, v58] = state;
        visitor(v0, v1, v2, v3, v4, v5, v6, v7, v8, v9, v10, v11, v12, v13, v14, v15, v16, v17, v18,
                v19, v20, v21, v22, v23, v24, v25, v26, v27, v28, v29, v30, v31, v32, v33, v34, v35,
                v36, v37, v38, v39, v40, v41, v42, v43, v44, v45, v46, v47, v48, v49, v50, v51, v52,
                v53, v54, v55, v56, v57, v58);
    } else if constexpr (Count == 60) {
        const auto& [v0, v1, v2, v3, v4, v5, v6, v7, v8, v9, v10, v11, v12, v13, v14, v15, v16, v17,
                     v18, v19, v20, v21, v22, v23, v24, v25, v26, v27, v28, v29, v30, v31, v32, v33,
                     v34, v35, v36, v37, v38, v39, v40, v41, v42, v43, v44, v45, v46, v47, v48, v49,
                     v50, v51, v52, v53, v54, v55, v56, v57, v58, v59] = state;
        visitor(v0, v1, v2, v3, v4, v5, v6, v7, v8, v9, v10, v11, v12, v13, v14, v15, v16, v17, v18,
                v19, v20, v21, v22, v23, v24, v25, v26, v27, v28, v29, v30, v31, v32, v33, v34, v35,
                v36, v37, v38, v39, v40, v41, v42, v43, v44, v45, v46, v47, v48, v49, v50, v51, v52,
                v53, v54, v55, v56, v57, v58, v59);
    } else if constexpr (Count == 61) {
        const auto& [v0, v1, v2, v3, v4, v5, v6, v7, v8, v9, v10, v11, v12, v13, v14, v15, v16, v17,
                     v18, v19, v20, v21, v22, v23, v24, v25, v26, v27, v28, v29, v30, v31, v32, v33,
                     v34, v35, v36, v37, v38, v39, v40, v41, v42, v43, v44, v45, v46, v47, v48, v49,
                     v50, v51, v52, v53, v54, v55, v56, v57, v58, v59, v60] = state;
        visitor(v0, v1, v2, v3, v4, v5, v6, v7, v8, v9, v10, v11, v12, v13, v14, v15, v16, v17, v18,
                v19, v20, v21, v22, v23, v24, v25, v26, v27, v28, v29, v30, v31, v32, v33, v34, v35,
                v36, v37, v38, v39, v40, v41, v42, v43, v44, v45, v46, v47, v48, v49, v50, v51, v52,
                v53, v54, v55, v56, v57, v58, v59, v60);
    } else if constexpr (Count == 62) {
        const auto& [v0, v1, v2, v3, v4, v5, v6, v7, v8, v9, v10, v11, v12, v13, v14, v15, v16, v17,
                     v18, v19, v20, v21, v22, v23, v24, v25, v26, v27, v28, v29, v30, v31, v32, v33,
                     v34, v35, v36, v37, v38, v39, v40, v41, v42, v43, v44, v45, v46, v47, v48, v49,
                     v50, v51, v52, v53, v54, v55, v56, v57, v58, v59, v60, v61] = state;
        visitor(v0, v1, v2, v3, v4, v5, v6, v7, v8, v9, v10, v11, v12, v13, v14, v15, v16, v17, v18,
                v19, v20, v21, v22, v23, v24, v25, v26, v27, v28, v29, v30, v31, v32, v33, v34, v35,
                v36, v37, v38, v39, v40, v41, v42, v43, v44, v45, v46, v47, v48, v49, v50, v51, v52,
                v53, v54, v55, v56, v57, v58, v59, v60, v61);
    } else if constexpr (Count == 63) {
        const auto& [v0, v1, v2, v3, v4, v5, v6, v7, v8, v9, v10, v11, v12, v13, v14, v15, v16, v17,
                     v18, v19, v20, v21, v22, v23, v24, v25, v26, v27, v28, v29, v30, v31, v32, v33,
                     v34, v35, v36, v37, v38, v39, v40, v41, v42, v43, v44, v45, v46, v47, v48, v49,
                     v50, v51, v52, v53, v54, v55, v56, v57, v58, v59, v60, v61, v62] = state;
        visitor(v0, v1, v2, v3, v4, v5, v6, v7, v8, v9, v10, v11, v12, v13, v14, v15, v16, v17, v18,
                v19, v20, v21, v22, v23, v24, v25, v26, v27, v28, v29, v30, v31, v32, v33, v34, v35,
                v36, v37, v38, v39, v40, v41, v42, v43, v44, v45, v46, v47, v48, v49, v50, v51, v52,
                v53, v54, v55, v56, v57, v58, v59, v60, v61, v62);
    } else if constexpr (Count == 64) {
        const auto& [v0, v1, v2, v3, v4, v5, v6, v7, v8, v9, v10, v11, v12, v13, v14, v15, v16, v17,
                     v18, v19, v20, v21, v22, v23, v24, v25, v26, v27, v28, v29, v30, v31, v32, v33,
                     v34, v35, v36, v37, v38, v39, v40, v41, v42, v43, v44, v45, v46, v47, v48, v49,
                     v50, v51, v52, v53, v54, v55, v56, v57, v58, v59, v60, v61, v62, v63] = state;
        visitor(v0, v1, v2, v3, v4, v5, v6, v7, v8, v9, v10, v11, v12, v13, v14, v15, v16, v17, v18,
                v19, v20, v21, v22, v23, v24, v25, v26, v27, v28, v29, v30, v31, v32, v33, v34, v35,
                v36, v37, v38, v39, v40, v41, v42, v43, v44, v45, v46, v47, v48, v49, v50, v51, v52,
                v53, v54, v55, v56, v57, v58, v59, v60, v61, v62, v63);
    }
}
} // namespace gdl
