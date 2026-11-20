#include "omsi/spline_memory_pool.hpp"

namespace omsi::spline {

SplineMeshArena& get_thread_spline_arena() {
    thread_local SplineMeshArena s_arena(16 * 1024 * 1024); // 16MB per worker thread arena
    return s_arena;
}

} // namespace omsi::spline
