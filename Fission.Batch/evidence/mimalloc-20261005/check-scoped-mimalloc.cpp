//
// Created by Dottik on 5/10/2026.
//
#include "FissionAllocator.hpp"
#include <cassert>
#include <cstdlib>
#include <vector>

struct alignas(128) Object { int value = 7; };

int main() {
    auto own = Fission::MakeShared<Object>();
    assert(own->value == 7 && mi_is_in_heap_region(own.get()));
    assert(reinterpret_cast<uintptr_t>(own.get()) % alignof(Object) == 0);
    auto standard = std::make_shared<Object>();
    assert(!mi_is_in_heap_region(standard.get()));
    auto raw = std::malloc(42);
    assert(raw && !mi_is_in_heap_region(raw));
    std::free(raw);
    std::vector<int, Fission::Allocator<int>> downstream(128, 9);
    assert(mi_is_in_heap_region(downstream.data()) && downstream.back() == 9);
    std::weak_ptr<Object> weak = own;
    own.reset();
    assert(weak.expired());
}
