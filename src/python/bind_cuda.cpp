#if defined(ENOKI_ENABLE_JIT)
#include "bind.h"
#include "random.h"
#include "loop.h"
#include <enoki/cuda.h>
#include <enoki/autodiff.h>

using Guide = ek::CUDAArray<float>;

void export_cuda(py::module_ &m) {
    py::module_ cuda = m.def_submodule("cuda");

    ENOKI_BIND_ARRAY_BASE(cuda, Guide, false);
    ENOKI_BIND_ARRAY_TYPES(cuda, Guide, false);

    bind_pcg32<Guide>(cuda);

    py::class_<Loop<Guide>, ek::LoopBase> loop(cuda, "Loop");
    loop.def(py::init<py::args>())
        .def("put", &Loop<Guide>::put)
        .def("init", &Loop<Guide>::init)
        .def("cond", &Loop<Guide>::cond)
        .def("mask", &Loop<Guide>::mask);

#if defined(ENOKI_ENABLE_AUTODIFF)
    loop.def("cond", [](Loop<Guide> &g,
                        const ek::DiffArray<ek::CUDAArray<bool>> &mask) {
        return g.cond(ek::detach(mask));
    });
#endif
}
#endif
