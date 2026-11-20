#include "gfx/dx12/common/dx12_common.hpp"

namespace omsi::gfx::dx12 {

void throw_hr(HRESULT hr, const char* what) {
    if (FAILED(hr)) throw std::runtime_error(what);
}

} // namespace omsi::gfx::dx12
