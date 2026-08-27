#include "ui/ui_services.hpp"

namespace wowee {
namespace ui {

namespace {
/// By value: the struct is a handful of pointers and two callables, and a copy
/// cannot dangle behind a caller that rebuilds its own.
UIServices& stored() {
    static UIServices services;
    return services;
}
}  // namespace

void setUiServices(const UIServices& services) { stored() = services; }
const UIServices& uiServices() { return stored(); }

}  // namespace ui
}  // namespace wowee
