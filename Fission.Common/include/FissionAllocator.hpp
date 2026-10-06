//
// Created by Dottik on 5/10/2026.
//
#pragma once
#include <memory>
#include <utility>

#ifdef FISSION_USE_MIMALLOC
#include <mimalloc.h>
#endif

namespace Fission {
    template <typename T>
    using Allocator =
#ifdef FISSION_USE_MIMALLOC
        mi_stl_allocator<T>;
#else
        std::allocator<T>;
#endif

    template <typename T, typename... Args> std::shared_ptr<T> MakeShared(Args &&...args) {
#ifdef FISSION_USE_MIMALLOC
        return std::allocate_shared<T>(Allocator<T>{}, std::forward<Args>(args)...);
#else
        return std::make_shared<T>(std::forward<Args>(args)...);
#endif
    }
} // namespace Fission
